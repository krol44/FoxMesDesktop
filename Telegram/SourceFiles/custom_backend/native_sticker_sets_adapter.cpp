/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_sticker_sets_adapter.h"

#include "api/api_hash.h"
#include "apiwrap.h"
#include "base/flat_map.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "custom_backend/native_streaming_loader.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"

#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
#include <QtCore/QUuid>

namespace CustomBackend::StickerSets {
namespace {

constexpr auto kStickerBit = DocumentId(1) << 44;
constexpr auto kInstallDate = TimeId(1);
constexpr auto kDocumentDate = TimeId(1);
constexpr auto kFeaturedCovers = 5;
constexpr auto kDocumentsPerRequest = 200;
constexpr auto kBackgroundEmojiFirst = DocumentId(520093696);
constexpr auto kBackgroundEmojiLast = kBackgroundEmojiFirst + 65535;
struct Set final {
	uint64 id = 0;
	bool emoji = false;
	QString title;
	QString shortName;
	std::vector<Item> items;
	int32 hash = 0;
};

struct Group final {
	QString title;
	DocumentId icon = 0;
	QVector<MTPstring> emoticons;
};

struct State final {
	std::vector<Set> sets;
	std::vector<uint64> installedEmoji;
	std::vector<uint64> installedStickers;
	qint64 revision = -1;
	bool loaded = false;
	bool loading = false;
	std::vector<Fn<void(bool)>> waiting;
	base::flat_map<DocumentId, Item> items;
	base::flat_map<DocumentId, Item> foreign;
	std::vector<Group> groups;
	rpl::event_stream<> updated;
};

base::flat_map<not_null<Main::Session*>, std::unique_ptr<State>> &States() {
	static auto value = base::flat_map<
		not_null<Main::Session*>,
		std::unique_ptr<State>>();
	return value;
}

[[nodiscard]] State &StateFor(not_null<Main::Session*> session) {
	auto &slot = States()[session];
	if (!slot) {
		slot = std::make_unique<State>();
	}
	return *slot;
}

[[nodiscard]] State *FindState(Main::Session *session) {
	const auto i = session ? States().find(session) : States().end();
	return (i == States().end()) ? nullptr : i->second.get();
}

[[nodiscard]] std::vector<uint64> &Installed(State &state, bool emoji) {
	return emoji ? state.installedEmoji : state.installedStickers;
}

[[nodiscard]] bool IsInstalled(const State &state, const Set &set) {
	return ranges::contains(
		set.emoji ? state.installedEmoji : state.installedStickers,
		set.id);
}

[[nodiscard]] std::vector<uint64> ParseIds(const QJsonValue &value) {
	auto result = std::vector<uint64>();
	for (const auto &entry : value.toArray()) {
		const auto id = entry.toVariant().toLongLong();
		if (id > 0) {
			result.push_back(uint64(id));
		}
	}
	return result;
}

[[nodiscard]] int32 SetHash(const Set &set) {
	auto result = Api::HashInit();
	for (const auto &item : set.items) {
		Api::HashUpdate(result, uint64(item.id));
		Api::HashUpdate(result, uint64(qHash(item.url)));
	}
	return int32(Api::HashFinalize(result) & 0x7FFFFFFF);
}

[[nodiscard]] MTPDocument BuildDocument(const Item &item) {
	const auto webm = (item.mime == u"video/webm"_q);
	const auto lottie = (item.mime == u"application/x-tgsticker"_q);
	const auto mime = webm
		? u"video/webm"_q
		: lottie
		? u"application/x-tgsticker"_q
		: u"image/webp"_q;
	const auto fallback = item.sticker ? 512 : 100;
	const auto width = (item.width > 0) ? item.width : fallback;
	const auto height = (item.height > 0) ? item.height : fallback;
	auto attributes = QVector<MTPDocumentAttribute>{
		MTP_documentAttributeFilename(MTP_string(
			(item.sticker ? u"sticker"_q : u"emoji"_q)
				+ (webm ? u".webm"_q : lottie ? u".tgs"_q : u".webp"_q))),
	};
	if (webm) {
		attributes.push_back(MTP_documentAttributeVideo(
			MTP_flags(0),
			MTP_double(3.),
			MTP_int(width),
			MTP_int(height),
			MTPint(),
			MTPdouble(),
			MTPstring()));
	} else {
		attributes.push_back(MTP_documentAttributeImageSize(
			MTP_int(width),
			MTP_int(height)));
	}
	const auto set = item.setId
		? MTP_inputStickerSetID(MTP_long(item.setId), MTP_long(0))
		: MTP_inputStickerSetEmpty();
	if (item.sticker) {
		attributes.push_back(MTP_documentAttributeSticker(
			MTP_flags(0),
			MTP_string(item.emoji),
			set,
			MTPMaskCoords()));
	} else {
		using Flag = MTPDdocumentAttributeCustomEmoji::Flag;
		attributes.push_back(MTP_documentAttributeCustomEmoji(
			MTP_flags(Flag::f_free | (item.textColor ? Flag::f_text_color : Flag())),
			MTP_string(item.emoji),
			set));
	}
	return MTP_document(
		MTP_flags(0),
		MTP_long(DocumentIdFor(item)),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(kDocumentDate),
		MTP_string(mime),
		MTP_long(item.size),
		MTPVector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(0),
		MTP_vector<MTPDocumentAttribute>(attributes));
}

void RememberUrl(not_null<Main::Session*> session, const Item &item) {
	if (item.url.isEmpty()) {
		return;
	}
	const auto document = session->data().document(DocumentIdFor(item));
	document->setContentUrl(item.url);
	Streaming::RememberSource(document, item.url);
	if (item.mime == u"image/webp"_q && item.width > 0 && item.height > 0) {
		document->updateThumbnails(
			InlineImageLocation(),
			ImageWithLocation{
				.location = ImageLocation(
					DownloadLocation{ PlainUrlLocation{ item.url } },
					item.width,
					item.height),
			},
			ImageWithLocation(),
			false);
	}
}

[[nodiscard]] MTPStickerSet BuildSet(const State &state, const Set &set) {
	using Flag = MTPDstickerSet::Flag;
	const auto installed = IsInstalled(state, set);
	auto flags = Flag::f_official | Flag();
	if (set.emoji) {
		flags |= Flag::f_emojis;
	}
	if (installed) {
		flags |= Flag::f_installed_date;
	}
	if (!set.items.empty()) {
		flags |= Flag::f_thumb_document_id;
	}
	return MTP_stickerSet(
		MTP_flags(flags),
		MTP_int(installed ? kInstallDate : 0),
		MTP_long(set.id),
		MTP_long(0),
		MTP_string(set.title),
		MTP_string(set.shortName),
		MTPVector<MTPPhotoSize>(),
		MTPint(),
		MTPint(),
		MTP_long(set.items.empty() ? 0 : DocumentIdFor(set.items.front())),
		MTP_int(int(set.items.size())),
		MTP_int(set.hash));
}

[[nodiscard]] MTPmessages_StickerSet BuildFullSet(
		not_null<Main::Session*> session,
		const State &state,
		const Set &set) {
	auto packs = QVector<MTPStickerPack>();
	auto documents = QVector<MTPDocument>();
	for (const auto &item : set.items) {
		RememberUrl(session, item);
		documents.push_back(BuildDocument(item));
		packs.push_back(MTP_stickerPack(
			MTP_string(item.emoji),
			MTP_vector<MTPlong>(QVector<MTPlong>{ MTP_long(DocumentIdFor(item)) })));
	}
	return MTP_messages_stickerSet(
		BuildSet(state, set),
		MTP_vector<MTPStickerPack>(packs),
		MTPVector<MTPStickerKeyword>(),
		MTP_vector<MTPDocument>(documents));
}

[[nodiscard]] const Set *FindSet(const State &state, uint64 id) {
	const auto i = ranges::find(state.sets, id, &Set::id);
	return (i == state.sets.end()) ? nullptr : &*i;
}

[[nodiscard]] const Set *FindSet(const State &state, const QString &shortName) {
	const auto i = ranges::find_if(state.sets, [&](const Set &set) {
		return !set.shortName.compare(shortName, Qt::CaseInsensitive);
	});
	return (i == state.sets.end()) ? nullptr : &*i;
}

[[nodiscard]] uint64 SetIdFromInput(const MTPInputStickerSet &input) {
	return (input.type() == mtpc_inputStickerSetID)
		? uint64(input.c_inputStickerSetID().vid().v)
		: 0;
}

[[nodiscard]] QString ShortNameFromInput(const MTPInputStickerSet &input) {
	return (input.type() == mtpc_inputStickerSetShortName)
		? qs(input.c_inputStickerSetShortName().vshort_name())
		: QString();
}

[[nodiscard]] bool OwnsInput(const MTPInputStickerSet &input) {
	const auto id = SetIdFromInput(input);
	return ((id > 0) && (id <= uint64(0x7FFFFFFF)))
		|| !ShortNameFromInput(input).isEmpty();
}

[[nodiscard]] uint64 ResolveSetId(
		const State &state,
		uint64 setId,
		const QString &shortName) {
	if (setId || shortName.isEmpty()) {
		return setId;
	}
	const auto set = FindSet(state, shortName);
	return set ? set->id : 0;
}

void Apply(
		not_null<Main::Session*> session,
		const QJsonObject &value) {
	auto &state = StateFor(session);
	auto sets = std::vector<Set>();
	auto items = base::flat_map<DocumentId, Item>();
	for (const auto &entry : value.value("sets").toArray()) {
		const auto object = entry.toObject();
		auto set = Set{
			.id = uint64(object.value("id").toVariant().toLongLong()),
			.emoji = (object.value("kind").toString() == u"emoji"_q),
			.title = object.value("title").toString(),
			.shortName = object.value("short_name").toString(),
		};
		if (!set.id) {
			continue;
		}
		for (const auto &raw : object.value("items").toArray()) {
			if (auto item = ParseItem(raw.toObject(), !set.emoji)) {
				items.emplace(item->id, *item);
				set.items.push_back(std::move(*item));
			}
		}
		set.hash = SetHash(set);
		sets.push_back(std::move(set));
	}
	state.sets = std::move(sets);
	state.items = std::move(items);
	state.groups.clear();
	for (const auto &entry : value.value("groups").toArray()) {
		const auto object = entry.toObject();
		const auto icon = ParseItem(object.value("icon").toObject(), false);
		if (!icon) {
			continue;
		}
		state.items.emplace(icon->id, *icon);
		auto group = Group{
			.title = object.value("title").toString(),
			.icon = DocumentIdFor(*icon),
		};
		for (const auto &emoticon : object.value("emoticons").toArray()) {
			group.emoticons.push_back(MTP_string(emoticon.toString()));
		}
		state.groups.push_back(std::move(group));
	}
	const auto installed = value.value("installed").toObject();
	const auto revision = value.value("revision").toVariant().toLongLong();
	if (revision >= state.revision) {
		state.installedEmoji = ParseIds(installed.value("emoji"));
		state.installedStickers = ParseIds(installed.value("sticker"));
		state.revision = revision;
	}
	for (const auto &[id, item] : state.items) {
		RememberUrl(session, item);
	}
	state.loaded = true;
}

void Finish(not_null<Main::Session*> session, bool ok) {
	auto &state = StateFor(session);
	state.loading = false;
	auto waiting = base::take(state.waiting);
	for (const auto &callback : waiting) {
		callback(ok);
	}
	if (ok) {
		state.updated.fire({});
	}
}

void WhenLoaded(not_null<Main::Session*> session, Fn<void(bool)> done) {
	auto &state = StateFor(session);
	if (state.loaded) {
		done(true);
		return;
	}
	state.waiting.push_back(std::move(done));
	if (state.loading) {
		return;
	}
	state.loading = true;
	const auto weak = base::make_weak(session);
	ClientFor(session).stickerSets([weak](
			QJsonDocument doc,
			QString error,
			int status) {
		const auto session = weak.get();
		if (!session || !FindState(session)) {
			return;
		}
		const auto ok = error.isEmpty() && status == 200 && doc.isObject();
		if (ok) {
			Apply(session, doc.object());
		}
		Finish(session, ok);
	});
}

void ReceiveState(not_null<Main::Session*> session, const QJsonObject &value) {
	auto &state = StateFor(session);
	if (!value.value("revision").isDouble()) {
		return;
	}
	const auto revision = value.value("revision").toVariant().toLongLong();
	if (revision < state.revision) {
		return;
	}
	const auto installed = value.value("installed").toObject();
	state.installedEmoji = ParseIds(installed.value("emoji"));
	state.installedStickers = ParseIds(installed.value("sticker"));
	state.revision = revision;
}

void Refresh(not_null<Main::Session*> session) {
	auto &stickers = session->data().stickers();
	stickers.setLastUpdate(0);
	stickers.setLastFeaturedUpdate(0);
	session->api().updateStickers();
	stickers.setLastEmojiUpdate(0);
	stickers.setLastFeaturedEmojiUpdate(0);
	session->api().updateCustomEmoji();
}

void Mutate(
		not_null<Main::Session*> session,
		const QString &operation,
		Fn<void(Fn<void(QJsonDocument, QString, int)>)> send,
		Mtp::Answer answer,
		Fn<void(Mtp::Answer)> success) {
	const auto weak = base::make_weak(session);
	send([=](QJsonDocument doc, QString error, int status) {
		const auto session = weak.get();
		if (!session || !FindState(session)) {
			answer.fail(u"SESSION_REVOKED"_q, 401);
			return;
		}
		if (error.isEmpty() && status == 200 && doc.isObject()) {
			ReceiveState(session, doc.object());
			StateFor(session).updated.fire({});
			success(answer);
			return;
		}
		if (status > 0 && status < 500) {
			answer.fail(Mtp::ErrorType(
				doc,
				error,
				status,
				u"STICKERSET_INVALID"_q,
				u"STICKERSET_INVALID"_q), status);
			return;
		}
		ClientFor(session).operationResult(operation, [=](
				QJsonDocument recovered,
				QString lookupError,
				int) {
			const auto session = weak.get();
			if (!session || !FindState(session)) {
				answer.fail(u"SESSION_REVOKED"_q, 401);
				return;
			}
			const auto value = recovered.object();
			if (lookupError.isEmpty()
				&& value.value("status").toString() == u"done"_q
				&& value.value("result").isObject()) {
				ReceiveState(session, value.value("result").toObject());
				StateFor(session).updated.fire({});
				success(answer);
			} else {
				answer.fail(u"STICKERSET_INSTALL_FAILED"_q, 500);
			}
		});
	});
}

void SetInstalled(
		not_null<Main::Session*> session,
		uint64 setId,
		bool installed,
		Mtp::Answer answer,
		Fn<void(Mtp::Answer)> success) {
	const auto operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
	Mutate(session, operation, [=](auto done) {
		ClientFor(session).setStickerSetInstalled(
			qint64(setId),
			installed,
			operation,
			std::move(done));
	}, answer, success);
}

[[nodiscard]] MTPmessages_AllStickers BuildInstalled(
		const State &state,
		bool emoji,
		uint64 requestHash) {
	auto sets = QVector<MTPStickerSet>();
	auto hash = Api::HashInit();
	for (const auto id : emoji ? state.installedEmoji : state.installedStickers) {
		if (const auto set = FindSet(state, id); set && set->emoji == emoji) {
			sets.push_back(BuildSet(state, *set));
			Api::HashUpdate(hash, set->hash);
		}
	}
	const auto finalized = Api::HashFinalize(hash);
	if (requestHash && requestHash == finalized) {
		return MTP_messages_allStickersNotModified();
	}
	return MTP_messages_allStickers(
		MTP_long(finalized),
		MTP_vector<MTPStickerSet>(sets));
}

[[nodiscard]] MTPmessages_FeaturedStickers BuildFeatured(
		not_null<Main::Session*> session,
		const State &state,
		bool emoji) {
	auto sets = QVector<MTPStickerSetCovered>();
	auto hash = Api::HashInit();
	for (const auto &set : state.sets) {
		if (set.emoji != emoji || IsInstalled(state, set)) {
			continue;
		}
		Api::HashUpdate(hash, set.id);
		if (emoji) {
			const auto full = BuildFullSet(session, state, set);
			const auto &data = full.c_messages_stickerSet();
			sets.push_back(MTP_stickerSetFullCovered(
				data.vset(),
				data.vpacks(),
				data.vkeywords(),
				data.vdocuments()));
		} else {
			auto covers = QVector<MTPDocument>();
			for (const auto &item : set.items) {
				if (covers.size() >= kFeaturedCovers) {
					break;
				}
				RememberUrl(session, item);
				covers.push_back(BuildDocument(item));
			}
			sets.push_back(MTP_stickerSetMultiCovered(
				BuildSet(state, set),
				MTP_vector<MTPDocument>(covers)));
		}
	}
	using Flag = MTPDmessages_featuredStickers::Flag;
	return MTP_messages_featuredStickers(
		MTP_flags(emoji ? Flag::f_premium : Flag()),
		MTP_long(Api::HashFinalize(hash)),
		MTP_int(sets.size()),
		MTP_vector<MTPStickerSetCovered>(sets),
		MTPVector<MTPlong>());
}

[[nodiscard]] MTPmessages_FoundStickerSets BuildFound(
		not_null<Main::Session*> session,
		const State &state,
		bool emoji,
		const QString &query) {
	auto sets = QVector<MTPStickerSetCovered>();
	const auto needle = query.trimmed();
	for (const auto &set : state.sets) {
		if (set.emoji != emoji
			|| needle.isEmpty()
			|| !set.title.contains(needle, Qt::CaseInsensitive)) {
			continue;
		}
		const auto full = BuildFullSet(session, state, set);
		const auto &data = full.c_messages_stickerSet();
		sets.push_back(MTP_stickerSetFullCovered(
			data.vset(),
			data.vpacks(),
			data.vkeywords(),
			data.vdocuments()));
	}
	return MTP_messages_foundStickerSets(
		MTP_long(0),
		MTP_vector<MTPStickerSetCovered>(sets));
}

[[nodiscard]] QString BareEmoji(QString value) {
	return value.remove(QChar(0xFE0F)).trimmed();
}

[[nodiscard]] MTPmessages_Stickers BuildByEmoji(
		not_null<Main::Session*> session,
		const State &state,
		const QString &emoticon) {
	const auto needle = BareEmoji(emoticon);
	auto order = std::vector<uint64>(state.installedStickers);
	for (const auto &set : state.sets) {
		if (!set.emoji && !ranges::contains(order, set.id)) {
			order.push_back(set.id);
		}
	}
	auto documents = QVector<MTPDocument>();
	for (const auto id : order) {
		const auto set = FindSet(state, id);
		if (!set || set->emoji) {
			continue;
		}
		for (const auto &item : set->items) {
			if (!needle.isEmpty() && BareEmoji(item.emoji) == needle) {
				RememberUrl(session, item);
				documents.push_back(BuildDocument(item));
			}
		}
	}
	return MTP_messages_stickers(
		MTP_long(0),
		MTP_vector<MTPDocument>(documents));
}

[[nodiscard]] MTPmessages_FoundStickers BuildSearch(
		not_null<Main::Session*> session,
		const State &state,
		bool emoji,
		const QString &query,
		const QString &emoticon) {
	const auto haystack = BareEmoji(emoticon + query);
	const auto needle = query.trimmed();
	auto documents = QVector<MTPDocument>();
	for (const auto &set : state.sets) {
		if (set.emoji != emoji) {
			continue;
		}
		const auto byTitle = !needle.isEmpty()
			&& set.title.contains(needle, Qt::CaseInsensitive);
		for (const auto &item : set.items) {
			const auto alt = BareEmoji(item.emoji);
			if (byTitle || (!alt.isEmpty() && haystack.contains(alt))) {
				RememberUrl(session, item);
				documents.push_back(BuildDocument(item));
			}
		}
	}
	return MTP_messages_foundStickers(
		MTP_flags(0),
		MTPint(),
		MTP_long(0),
		MTP_vector<MTPDocument>(documents));
}

[[nodiscard]] MTPmessages_EmojiGroups BuildGroups(const State &state) {
	auto groups = QVector<MTPEmojiGroup>();
	for (const auto &group : state.groups) {
		groups.push_back(MTP_emojiGroup(
			MTP_string(group.title),
			MTP_long(group.icon),
			MTP_vector<MTPstring>(group.emoticons)));
	}
	return MTP_messages_emojiGroups(
		MTP_int(0),
		MTP_vector<MTPEmojiGroup>(groups));
}

void ResolveDocuments(
		not_null<Main::Session*> session,
		std::vector<DocumentId> ids,
		Fn<void(QVector<MTPDocument>)> done) {
	WhenLoaded(session, [=](bool) {
		auto &state = StateFor(session);
		auto missing = QList<qint64>();
		for (const auto id : ids) {
			if (!state.items.contains(id) && !state.foreign.contains(id)) {
				missing.push_back(qint64(id));
			}
		}
		const auto finish = [=] {
			const auto owner = FindState(session);
			auto result = QVector<MTPDocument>();
			for (const auto id : ids) {
				const Item *item = nullptr;
				if (owner) {
					if (const auto i = owner->items.find(id); i != owner->items.end()) {
						item = &i->second;
					} else if (const auto j = owner->foreign.find(id); j != owner->foreign.end()) {
						item = &j->second;
					}
				}
				if (item) {
					RememberUrl(session, *item);
					result.push_back(BuildDocument(*item));
				} else {
					result.push_back(MTP_documentEmpty(MTP_long(id)));
				}
			}
			done(std::move(result));
		};
		if (missing.isEmpty()) {
			finish();
			return;
		}
		const auto weak = base::make_weak(session);
		const auto batches = std::make_shared<int>(0);
		for (auto from = 0; from < missing.size(); from += kDocumentsPerRequest) {
			++*batches;
			ClientFor(session).emojiDocuments(
				missing.mid(from, kDocumentsPerRequest),
				[=](QJsonDocument doc, QString error, int status) {
					if (const auto session = weak.get()) {
						if (const auto owner = FindState(session)) {
							if (error.isEmpty() && status == 200) {
								for (const auto &entry : doc.object().value("items").toArray()) {
									if (auto item = ParseItem(entry.toObject(), false)) {
										owner->foreign.emplace(item->id, *item);
									}
								}
							}
						}
					}
					if (!--*batches) {
						finish();
					}
				});
		}
	});
}

[[nodiscard]] bool IsBackgroundEmoji(DocumentId id) {
	return (id >= kBackgroundEmojiFirst) && (id <= kBackgroundEmojiLast);
}

void Handle(
		not_null<Main::Session*> session,
		mtpTypeId type,
		Mtp::Answer answer,
		uint64 hash,
		uint64 setId,
		bool flag,
		bool emoji,
		std::vector<uint64> list,
		QString text) {
	switch (type) {
	case mtpc_messages_getAllStickers:
	case mtpc_messages_getEmojiStickers:
		WhenLoaded(session, [=](bool ok) {
			const auto state = FindState(session);
			if (!ok || !state) {
				answer.fail(u"STICKERSETS_UNAVAILABLE"_q, 500);
				return;
			}
			answer.done(BuildInstalled(
				*state,
				(type == mtpc_messages_getEmojiStickers),
				hash));
		});
		return;
	case mtpc_messages_getMaskStickers:
		answer.done(MTP_messages_allStickers(
			MTP_long(0),
			MTPVector<MTPStickerSet>()));
		return;
	case mtpc_messages_getStickerSet:
		WhenLoaded(session, [=](bool ok) {
			const auto state = FindState(session);
			const auto set = (ok && state)
				? FindSet(*state, ResolveSetId(*state, setId, text))
				: nullptr;
			if (!set) {
				answer.fail(u"STICKERSET_INVALID"_q);
				return;
			}
			answer.done(BuildFullSet(session, *state, *set));
		});
		return;
	case mtpc_messages_getFeaturedStickers:
	case mtpc_messages_getFeaturedEmojiStickers:
	case mtpc_messages_getOldFeaturedStickers:
		WhenLoaded(session, [=](bool ok) {
			const auto state = FindState(session);
			if (!ok || !state) {
				answer.fail(u"STICKERSETS_UNAVAILABLE"_q, 500);
				return;
			}
			answer.done(BuildFeatured(
				session,
				*state,
				(type == mtpc_messages_getFeaturedEmojiStickers)));
		});
		return;
	case mtpc_messages_readFeaturedStickers:
		answer.done(MTP_boolTrue());
		return;
	case mtpc_messages_getArchivedStickers:
		answer.done(MTP_messages_archivedStickers(
			MTP_int(0),
			MTPVector<MTPStickerSetCovered>()));
		return;
	case mtpc_messages_getRecentStickers:
		answer.done(MTP_messages_recentStickers(
			MTP_long(0),
			MTPVector<MTPStickerPack>(),
			MTPVector<MTPDocument>(),
			MTPVector<MTPint>()));
		return;
	case mtpc_messages_installStickerSet:
	case mtpc_messages_uninstallStickerSet:
		WhenLoaded(session, [=](bool) {
			const auto state = FindState(session);
			const auto id = state ? ResolveSetId(*state, setId, text) : 0;
			if (!id) {
				answer.fail(u"STICKERSET_INVALID"_q);
			} else if (type == mtpc_messages_installStickerSet) {
				SetInstalled(session, id, !flag, answer, [](Mtp::Answer answer) {
					answer.done(MTP_messages_stickerSetInstallResultSuccess());
				});
			} else {
				SetInstalled(session, id, false, answer, [](Mtp::Answer answer) {
					answer.done(MTP_boolTrue());
				});
			}
		});
		return;
	case mtpc_messages_toggleStickerSets: {
		if (list.empty()) {
			answer.done(MTP_boolTrue());
			return;
		}
		const auto rest = std::vector<uint64>(list.begin() + 1, list.end());
		SetInstalled(session, list.front(), flag, answer, [=](Mtp::Answer answer) {
			if (rest.empty()) {
				answer.done(MTP_boolTrue());
			} else {
				Handle(session, type, answer, 0, 0, flag, emoji, rest, QString());
			}
		});
	} return;
	case mtpc_messages_reorderStickerSets: {
		const auto operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
		auto ids = QList<qint64>();
		for (const auto id : list) {
			ids.push_back(qint64(id));
		}
		Mutate(session, operation, [=](auto done) {
			ClientFor(session).orderStickerSets(
				emoji,
				ids,
				operation,
				std::move(done));
		}, answer, [](Mtp::Answer answer) {
			answer.done(MTP_boolTrue());
		});
	} return;
	case mtpc_messages_getCustomEmojiDocuments: {
		auto ids = std::vector<DocumentId>();
		for (const auto id : list) {
			if (!IsBackgroundEmoji(DocumentId(id))) {
				ids.push_back(DocumentId(id));
			}
		}
		ResolveDocuments(session, ids, [=](QVector<MTPDocument> documents) {
			answer.done(MTP_vector<MTPDocument>(documents));
		});
	} return;
	case mtpc_messages_getStickers:
		WhenLoaded(session, [=](bool) {
			const auto state = FindState(session);
			if (!state) {
				answer.fail(u"SESSION_REVOKED"_q, 401);
				return;
			}
			answer.done(BuildByEmoji(session, *state, text));
		});
		return;
	case mtpc_messages_searchStickers:
		WhenLoaded(session, [=](bool) {
			const auto state = FindState(session);
			if (!state) {
				answer.fail(u"SESSION_REVOKED"_q, 401);
				return;
			}
			const auto separator = text.indexOf(QChar(0));
			answer.done(BuildSearch(
				session,
				*state,
				emoji,
				text.mid(0, separator),
				text.mid(separator + 1)));
		});
		return;
	case mtpc_messages_getEmojiGroups:
	case mtpc_messages_getEmojiStatusGroups:
	case mtpc_messages_getEmojiProfilePhotoGroups:
	case mtpc_messages_getEmojiStickerGroups:
		WhenLoaded(session, [=](bool) {
			const auto state = FindState(session);
			if (!state) {
				answer.fail(u"SESSION_REVOKED"_q, 401);
				return;
			}
			answer.done(BuildGroups(*state));
		});
		return;
	case mtpc_messages_searchStickerSets:
	case mtpc_messages_searchEmojiStickerSets:
		WhenLoaded(session, [=](bool) {
			const auto state = FindState(session);
			if (!state) {
				answer.fail(u"SESSION_REVOKED"_q, 401);
				return;
			}
			answer.done(BuildFound(
				session,
				*state,
				(type == mtpc_messages_searchEmojiStickerSets),
				text));
		});
		return;
	}
	answer.fail(u"METHOD_INVALID"_q);
}

} // namespace

std::optional<Item> ParseItem(const QJsonObject &value, bool sticker) {
	const auto id = DocumentId(value.value("id").toVariant().toLongLong());
	const auto url = value.value("url").toString();
	if (!id || url.isEmpty()) {
		return std::nullopt;
	}
	return Item{
		.id = id,
		.emoji = value.value("emoji").toString(),
		.url = url,
		.mime = value.value("mime").toString(),
		.width = value.value("width").toInt(),
		.height = value.value("height").toInt(),
		.size = int64(value.value("size").toVariant().toLongLong()),
		.setId = uint64(value.value("set_id").toVariant().toLongLong()),
		.sticker = sticker,
		.textColor = value.value("text_color").toBool(),
	};
}

DocumentId DocumentIdFor(const Item &item) {
	return item.sticker ? (item.id | kStickerBit) : item.id;
}

DocumentId CatalogId(DocumentId documentId) {
	return documentId & ~kStickerBit;
}

bool IsCatalogSticker(DocumentId documentId) {
	return (documentId & kStickerBit) != 0;
}

MTPDocument Document(not_null<Main::Session*> session, const Item &item) {
	RememberUrl(session, item);
	return BuildDocument(item);
}

not_null<DocumentData*> Register(
		not_null<Main::Session*> session,
		const Item &item) {
	RememberUrl(session, item);
	return session->data().processDocument(BuildDocument(item));
}

std::optional<Item> Find(
		not_null<Main::Session*> session,
		DocumentId documentId) {
	const auto state = FindState(session);
	if (!state) {
		return std::nullopt;
	}
	const auto id = CatalogId(documentId);
	if (const auto i = state->items.find(id); i != state->items.end()) {
		if (i->second.sticker == IsCatalogSticker(documentId)) {
			return i->second;
		}
		return std::nullopt;
	}
	if (const auto i = state->foreign.find(id); i != state->foreign.end()) {
		return i->second;
	}
	return std::nullopt;
}

std::optional<MTPMessageMedia> MessageMedia(
		not_null<Main::Session*> session,
		const QJsonObject &sticker) {
	const auto item = ParseItem(sticker, true);
	if (!item) {
		return std::nullopt;
	}
	return MTP_messageMediaDocument(
		MTP_flags(MTPDmessageMediaDocument::Flag::f_document),
		Document(session, *item),
		MTPVector<MTPDocument>(),
		MTPPhoto(),
		MTPint(),
		MTPint());
}

void Load(not_null<Main::Session*> session) {
	WhenLoaded(session, [](bool) {});
}

void Reload(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	if (state.loading) {
		return;
	}
	state.loaded = false;
	const auto weak = base::make_weak(session);
	WhenLoaded(session, [weak](bool ok) {
		if (const auto session = weak.get(); session && ok) {
			Refresh(session);
		}
	});
}

rpl::producer<> Updated(not_null<Main::Session*> session) {
	return StateFor(session).updated.events();
}

void ApplyEvent(not_null<Main::Session*> session, const QJsonObject &state) {
	ReceiveState(session, state);
	StateFor(session).updated.fire({});
	Refresh(session);
}

bool Intercepts(const MTP::details::SerializedRequest &request) {
	switch (Mtp::RequestType(request)) {
	case mtpc_messages_getAllStickers:
	case mtpc_messages_getEmojiStickers:
	case mtpc_messages_getMaskStickers:
	case mtpc_messages_getFeaturedStickers:
	case mtpc_messages_getFeaturedEmojiStickers:
	case mtpc_messages_getOldFeaturedStickers:
	case mtpc_messages_readFeaturedStickers:
	case mtpc_messages_getArchivedStickers:
	case mtpc_messages_getRecentStickers:
	case mtpc_messages_toggleStickerSets:
	case mtpc_messages_reorderStickerSets:
	case mtpc_messages_getCustomEmojiDocuments:
	case mtpc_messages_getStickers:
	case mtpc_messages_searchStickerSets:
	case mtpc_messages_searchEmojiStickerSets:
	case mtpc_messages_searchStickers:
	case mtpc_messages_getEmojiGroups:
	case mtpc_messages_getEmojiStatusGroups:
	case mtpc_messages_getEmojiProfilePhotoGroups:
	case mtpc_messages_getEmojiStickerGroups:
		return true;
	case mtpc_messages_getStickerSet:
	case mtpc_messages_installStickerSet:
	case mtpc_messages_uninstallStickerSet: {
		auto reader = Mtp::Reader(request);
		const auto input = reader.read<MTPInputStickerSet>();
		return reader.ok() && OwnsInput(input);
	}
	}
	return false;
}

void Intercept(
		not_null<MTP::Instance*> instance,
		mtpRequestId requestId,
		const MTP::details::SerializedRequest &request) {
	const auto answer = Mtp::Answer(instance, requestId);
	const auto type = Mtp::RequestType(request);
	auto reader = Mtp::Reader(request);
	auto hash = uint64(0);
	auto setId = uint64(0);
	auto flag = false;
	auto emoji = false;
	auto list = std::vector<uint64>();
	auto text = QString();
	switch (type) {
	case mtpc_messages_getAllStickers:
	case mtpc_messages_getEmojiStickers:
		hash = uint64(reader.read<MTPlong>().v);
		break;
	case mtpc_messages_getStickerSet:
	case mtpc_messages_uninstallStickerSet: {
		const auto input = reader.read<MTPInputStickerSet>();
		setId = SetIdFromInput(input);
		text = ShortNameFromInput(input);
	} break;
	case mtpc_messages_installStickerSet: {
		const auto input = reader.read<MTPInputStickerSet>();
		setId = SetIdFromInput(input);
		text = ShortNameFromInput(input);
		flag = mtpIsTrue(reader.read<MTPBool>());
	} break;
	case mtpc_messages_toggleStickerSets: {
		const auto flags = reader.flags();
		flag = (flags & (1 << 2)) != 0;
		for (const auto &input : reader.read<MTPVector<MTPInputStickerSet>>().v) {
			if (const auto id = SetIdFromInput(input)) {
				list.push_back(id);
			}
		}
	} break;
	case mtpc_messages_reorderStickerSets: {
		const auto flags = reader.flags();
		emoji = (flags & (1 << 1)) != 0;
		if (flags & (1 << 0)) {
			answer.done(MTP_boolTrue());
			return;
		}
		for (const auto &id : reader.read<MTPVector<MTPlong>>().v) {
			list.push_back(uint64(id.v));
		}
	} break;
	case mtpc_messages_getCustomEmojiDocuments:
		for (const auto &id : reader.read<MTPVector<MTPlong>>().v) {
			list.push_back(uint64(id.v));
		}
		break;
	case mtpc_messages_getStickers:
		text = qs(reader.read<MTPstring>());
		break;
	case mtpc_messages_searchStickerSets:
	case mtpc_messages_searchEmojiStickerSets:
		reader.flags();
		text = qs(reader.read<MTPstring>());
		break;
	case mtpc_messages_searchStickers: {
		emoji = (reader.flags() & (1 << 0)) != 0;
		const auto query = qs(reader.read<MTPstring>());
		const auto emoticon = qs(reader.read<MTPstring>());
		text = query + QChar(0) + emoticon;
	} break;
	}
	if (!reader.ok()) {
		answer.fail(u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	crl::on_main([
		instance = QPointer<MTP::Instance>(instance.get()),
		answer,
		type,
		hash,
		setId,
		flag,
		emoji,
		list,
		text
	] {
		if (!instance) {
			return;
		}
		const auto session = Mtp::SessionFor(instance.data());
		if (!session || !BridgeFor(session)) {
			answer.fail(u"SESSION_REVOKED"_q, 401);
			return;
		}
		Handle(session, type, answer, hash, setId, flag, emoji, list, text);
	});
}

void ClearSession(not_null<Main::Session*> session) {
	States().remove(session);
}

}
