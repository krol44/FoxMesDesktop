/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_stickers_adapter.h"

#include "api/api_common.h"
#include "base/unixtime.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_reactions_adapter.h"
#include "custom_backend/native_runtime.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/text/text_entity.h"

namespace CustomBackend::Stickers {
namespace {

constexpr auto kStickerSide = 100;

constexpr auto kSetIdBase = uint64(0xF0C0000000000000ULL);

struct State {
	base::flat_map<DocumentId, DocumentId> stickers;
	base::flat_map<DocumentId, std::shared_ptr<Data::DocumentMedia>> media;
	base::flat_map<
		not_null<Data::StickersSet*>,
		std::shared_ptr<Data::StickersSetThumbnailView>> icons;
	std::vector<uint64> setIds;
	base::flat_map<DocumentId, QString> alts;
	rpl::lifetime lifetime;
	bool subscribed = false;
};

base::flat_map<not_null<Main::Session*>, State> &States() {
	static auto value = base::flat_map<not_null<Main::Session*>, State>();
	return value;
}

[[nodiscard]] State &StateFor(not_null<Main::Session*> session) {
	return States()[session];
}

[[nodiscard]] uint64 SetIdFor(const QString &category) {
	return kSetIdBase + (qHash(category) & 0xFFFFFFFFULL);
}

[[nodiscard]] DocumentId StickerDocumentId(uint64 setId, DocumentId emojiId) {
	return DocumentId(setId ^ emojiId);
}

[[nodiscard]] QString SetTitleFor(const QString &category) {
	const auto trimmed = category.trimmed();
	return trimmed.isEmpty()
		? tr::lng_stickers_default_set(tr::now)
		: trimmed;
}

[[nodiscard]] DocumentData *BuildDocument(
		not_null<Main::Session*> session,
		const Reactions::CatalogItem &item,
		uint64 setId,
		const Reactions::Asset &asset) {
	if (asset.content.isEmpty()) {
		return nullptr;
	}
	using Flag = MTPDdocumentAttributeSticker::Flag;
	const auto attributes = QVector<MTPDocumentAttribute>{
		MTP_documentAttributeFilename(MTP_string(u"sticker"_q)),
		MTP_documentAttributeImageSize(
			MTP_int(kStickerSide),
			MTP_int(kStickerSide)),
		MTP_documentAttributeSticker(
			MTP_flags(Flag()),
			MTP_string(item.emoji),
			MTP_inputStickerSetEmpty(),
			MTPMaskCoords()),
	};
	const auto id = StickerDocumentId(setId, item.id);
	const auto document = session->data().document(
		id,
		uint64(0),
		QByteArray(),
		base::unixtime::now(),
		attributes,
		asset.mime,
		InlineImageLocation(),
		ImageWithLocation(),
		ImageWithLocation(),
		false,
		0,
		int64(asset.content.size()));
	if (!document->sticker()) {
		return nullptr;
	}
	auto media = document->createMediaView();
	media->setBytes(asset.content);
	auto &state = StateFor(session);
	state.media[document->id] = std::move(media);
	state.stickers[document->id] = item.id;
	return document;
}

void SetIcon(
		not_null<Main::Session*> session,
		not_null<Data::StickersSet*> set,
		const Reactions::Asset &asset) {
	const auto type = asset.mime.startsWith(u"video/webm"_q)
		? StickerType::Webm
		: StickerType::Webp;
	set->setThumbnail(
		ImageWithLocation{
			.location = ImageLocation(
				DownloadLocation{ InMemoryLocation{ asset.content } },
				kStickerSide,
				kStickerSide),
			.bytes = asset.content,
			.bytesCount = int(asset.content.size()),
		},
		type);
	auto view = set->createThumbnailView();
	view->set(session, asset.content);
	StateFor(session).icons[set] = std::move(view);
}

void ApplyOne(
		not_null<Main::Session*> session,
		const Reactions::CatalogItem &item) {
	const auto asset = Reactions::AssetFor(session, item.id);
	if (asset.content.isEmpty()) {
		return;
	}
	auto &state = StateFor(session);
	if (const auto i = state.alts.find(item.id); i != state.alts.end()) {
		if (i->second == asset.mime) {
			return;
		}
		const auto setId = SetIdFor(item.category);
		const auto id = StickerDocumentId(setId, item.id);
		if (const auto j = state.media.find(id); j != state.media.end()) {
			j->second->setBytes(asset.content);
			i->second = asset.mime;
			const auto &sets = session->data().stickers().sets();
			if (const auto k = sets.find(setId); k != sets.end()) {
				const auto set = k->second.get();
				if (!set->stickers.isEmpty()
					&& set->stickers.front()->id == id) {
					SetIcon(session, set, asset);
				}
			}
			session->data().stickers().notifyUpdated(
				Data::StickersType::Stickers);
		}
		return;
	}
	auto &stickers = session->data().stickers();
	auto &sets = stickers.setsRef();
	auto &order = stickers.setsOrderRef();
	const auto setId = SetIdFor(item.category);
	auto i = sets.find(setId);
	if (i == sets.end()) {
		using SetFlag = Data::StickersSetFlag;
		i = sets.emplace(setId, std::make_unique<Data::StickersSet>(
			&session->data(),
			setId,
			uint64(0),
			uint64(0),
			SetTitleFor(item.category),
			QString(),
			0,
			SetFlag::Installed | SetFlag::Special,
			TimeId(0))).first;
		state.setIds.push_back(setId);
		order.push_back(setId);
	}
	const auto document = BuildDocument(session, item, setId, asset);
	if (!document) {
		return;
	}
	state.alts[item.id] = asset.mime;
	if (i->second->stickers.isEmpty()) {
		SetIcon(session, i->second.get(), asset);
	}
	i->second->stickers.push_back(document);
	i->second->count = i->second->stickers.size();
	stickers.setLastUpdate(crl::now());
	stickers.notifyUpdated(Data::StickersType::Stickers);
}

void Clear(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	auto &stickers = session->data().stickers();
	auto &sets = stickers.setsRef();
	auto &order = stickers.setsOrderRef();
	for (const auto setId : state.setIds) {
		if (const auto i = sets.find(setId); i != sets.end()) {
			state.icons.remove(i->second.get());
		}
		sets.remove(setId);
		order.removeOne(setId);
	}
	state.setIds.clear();
	state.alts.clear();
	state.media.clear();
	state.stickers.clear();
}

void Apply(not_null<Main::Session*> session) {
	for (const auto &item : Reactions::Catalog(session)) {
		ApplyOne(session, item);
	}
}

void Subscribe(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	if (state.subscribed) {
		return;
	}
	state.subscribed = true;
	const auto weak = base::make_weak(session);
	rpl::merge(
		Reactions::CatalogChanged(session),
		Reactions::AssetLoaded(session) | rpl::to_empty
	) | rpl::on_next([weak] {
		if (const auto strong = weak.get()) {
			Apply(strong);
		}
	}, state.lifetime);
}

} // namespace

void Request(not_null<Main::Session*> session) {
	Subscribe(session);
	Apply(session);
}

bool IsSticker(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document) {
	const auto &stickers = StateFor(session).stickers;
	return stickers.find(document->id) != stickers.end();
}

void ClearSession(not_null<Main::Session*> session) {
	if (States().find(session) == States().end()) {
		return;
	}
	Clear(session);
	States().remove(session);
}

bool Send(
		not_null<DocumentData*> document,
		const Api::SendAction &action) {
	const auto history = action.history;
	if (!history) {
		return false;
	}
	const auto session = &history->session();
	const auto &stickers = StateFor(session).stickers;
	const auto i = stickers.find(document->id);
	if (i == stickers.end()) {
		return false;
	}
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return false;
	}
	const auto emojiId = i->second;
	const auto emoji = Reactions::EmojiFor(session, emojiId);
	if (!emojiId || emoji.isEmpty()) {
		return false;
	}
	auto entities = EntitiesInText();
	entities.push_back(EntityInText(
		EntityType::CustomEmoji,
		0,
		int(emoji.size()),
		Data::SerializeCustomEmojiId(emojiId)));
	bridge->sendText(
		history,
		emoji,
		entities,
		Data::WebPageDraft(),
		ReplyTargetFrom(history, action.replyTo),
		std::nullopt,
		false,
		action.replyTo.topicRootId,
		PeerId(),
		QString(),
		SendOptionsFrom(action.options));
	return true;
}

}
