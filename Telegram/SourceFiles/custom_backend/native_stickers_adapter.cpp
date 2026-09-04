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

// Catalog assets are square by contract (emojis are stored 100x100), and the
// real frame size comes from the decoder at paint time anyway.
constexpr auto kStickerSide = 100;

// Set ids of our own, above every id upstream can mint from a stickerSet: an
// MTProto set id is a random int64 and a collision would make the panel show
// somebody's set under our title. Kept dense and derived from the category
// name so the same category keeps its id across refreshes and the panel does
// not scroll to the top on every catalog reload.
constexpr auto kSetIdBase = uint64(0xF0C0000000000000ULL);

struct State {
	// Documents built for this session, keyed by DocumentId, with the alt they
	// were built from. The alt is what a send puts on the wire.
	base::flat_map<DocumentId, QString> stickers;
	// Set ids this adapter installed, so a refresh replaces exactly them and
	// leaves anything else in Data::Stickers alone.
	std::vector<uint64> setIds;
	// Alts already in the panel, with the mime they were built from. The mime
	// is what a retry changes: an alt whose emoji_webm request failed first
	// gets the still webp, and the animation only arrives on a later attempt.
	base::flat_map<QString, QString> alts;
	rpl::lifetime lifetime;
	bool subscribed = false;
};

// The media views that own each sticker's bytes. A sticker document exists
// only in memory: nothing uploads or downloads it, so the only copy of its
// content is the one held here, and a view that died would take the picture
// with it.
base::flat_map<DocumentId, std::shared_ptr<Data::DocumentMedia>> &MediaCache() {
	static auto value = base::flat_map<
		DocumentId,
		std::shared_ptr<Data::DocumentMedia>>();
	return value;
}

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

// A separate id space from the reaction document of the same alt: they are two
// objects in one Data::Session and must not overwrite each other. Stable per
// (set, alt) so a refresh reuses the object the panel is already painting.
[[nodiscard]] DocumentId StickerDocumentId(uint64 setId, const QString &alt) {
	return DocumentId(setId ^ Reactions::RegisterDocumentId(alt));
}

[[nodiscard]] QString SetTitleFor(const QString &category) {
	const auto trimmed = category.trimmed();
	return trimmed.isEmpty()
		? tr::lng_stickers_default_set(tr::now)
		: trimmed;
}

// Builds the panel document for one catalog alt.
//
// documentAttributeSticker, not documentAttributeCustomEmoji: the sticker
// attribute is what makes DocumentData::setattributes() write
// StickersType::Stickers, and only a document of that type is laid out by
// StickersListWidget. The reaction strip needs the very same picture as a
// custom emoji instead, so both documents exist side by side over one download
// - see native_reactions_adapter.
[[nodiscard]] DocumentData *BuildDocument(
		not_null<Main::Session*> session,
		const QString &alt,
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
			// The alt travels on the document, which is where Send() reads it
			// back from: the wire protocol of this product speaks alts, not
			// document ids.
			MTP_string(alt),
			MTP_inputStickerSetEmpty(),
			MTPMaskCoords()),
	};
	const auto id = StickerDocumentId(setId, alt);
	const auto document = session->data().document(
		id,
		uint64(0), // access hash
		QByteArray(), // file reference
		base::unixtime::now(),
		attributes,
		asset.mime,
		InlineImageLocation(),
		ImageWithLocation(),
		ImageWithLocation(),
		false, // isPremiumSticker
		0, // dc
		int64(asset.content.size()));
	if (!document->sticker()) {
		return nullptr;
	}
	// Nothing ever uploads or downloads this document - the bytes came from
	// the catalog download - so without handing them to a media view that
	// stays alive, loaded() is false and the cell renders as an empty
	// placeholder forever.
	auto media = document->createMediaView();
	media->setBytes(asset.content);
	MediaCache()[document->id] = std::move(media);
	StateFor(session).stickers[document->id] = alt;
	return document;
}

// Adds the sticker for one alt, creating its set if this is the first entry of
// that category. Incremental on purpose: assets land one network reply at a
// time, and rebuilding the whole panel on each of them would be quadratic in
// the size of the catalog - and would throw away every document the open panel
// is currently painting.
void ApplyOne(
		not_null<Main::Session*> session,
		const Reactions::CatalogItem &item) {
	const auto asset = Reactions::AssetFor(item.emoji);
	if (asset.content.isEmpty()) {
		// Still downloading. AssetLoaded() brings us back here.
		return;
	}
	auto &state = StateFor(session);
	if (const auto i = state.alts.find(item.emoji); i != state.alts.end()) {
		if (i->second == asset.mime) {
			return;
		}
		// The still was replaced by the animation. The document keeps its id -
		// the panel is painting it right now - so only the bytes behind it are
		// swapped, and the next frame comes from the new content.
		const auto id = StickerDocumentId(SetIdFor(item.category), item.emoji);
		if (const auto j = MediaCache().find(id); j != MediaCache().end()) {
			j->second->setBytes(asset.content);
			i->second = asset.mime;
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
			uint64(0), // access hash
			uint64(0), // hash
			SetTitleFor(item.category),
			QString(), // short name
			0, // count
			SetFlag::Installed | SetFlag::Special,
			TimeId(0))).first;
		state.setIds.push_back(setId);
		order.push_back(setId);
	}
	const auto document = BuildDocument(session, item.emoji, setId, asset);
	if (!document) {
		return;
	}
	state.alts[item.emoji] = asset.mime;
	i->second->stickers.push_back(document);
	i->second->count = i->second->stickers.size();
	stickers.setLastUpdate(crl::now());
	stickers.notifyUpdated(Data::StickersType::Stickers);
}

// Drops every set this adapter installed. Only its own: the panel also holds
// special sections (recent, faved) that are not sets in this map at all, and
// clearing everything would be a promise about somebody else's data.
void Clear(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	auto &stickers = session->data().stickers();
	auto &sets = stickers.setsRef();
	auto &order = stickers.setsOrderRef();
	for (const auto setId : state.setIds) {
		sets.remove(setId);
		order.removeOne(setId);
	}
	state.setIds.clear();
	state.alts.clear();
	for (const auto &[id, alt] : state.stickers) {
		MediaCache().remove(id);
	}
	state.stickers.clear();
}

void Apply(not_null<Main::Session*> session) {
	for (const auto &item : Reactions::Catalog()) {
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
	// The catalog and its assets arrive over many requests, so the panel is
	// rebuilt as they land instead of once at an arbitrary moment. Upstream
	// does the same for a set whose thumbnail is still loading.
	rpl::merge(
		Reactions::CatalogChanged(),
		Reactions::AssetLoaded() | rpl::to_empty
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
	// The documents belong to the Data::Session going away; their bytes must
	// go with them, or the next login reuses views over dead objects.
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
	const auto alt = i->second;
	if (alt.isEmpty()) {
		return false;
	}
	// A sticker is a message whose whole text is one custom emoji, which is
	// exactly what fxl-web sends: the server marks such a message
	// singleCustomEmoji and both clients draw it large. It is not an
	// attachment - there is no file to upload, the asset is catalog data.
	auto entities = EntitiesInText();
	entities.push_back(EntityInText(
		EntityType::CustomEmoji,
		0,
		int(alt.size()),
		Data::SerializeCustomEmojiId(
			Reactions::RegisterDocumentId(alt))));
	bridge->sendText(
		history,
		alt,
		entities,
		Data::WebPageDraft(),
		ReplyTargetFrom(history, action.replyTo),
		std::nullopt,
		false,
		MsgId(),
		PeerId(),
		QString(),
		SendOptionsFrom(action.options));
	return true;
}

} // namespace CustomBackend::Stickers
