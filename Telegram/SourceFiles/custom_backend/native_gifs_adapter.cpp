/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_gifs_adapter.h"

#include "api/api_common.h"
#include "base/unixtime.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "history/history.h"
#include "main/main_session.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace CustomBackend::Gifs {
namespace {

// Its own id range, disjoint from the attachment range
// (kAttachmentMediaIdOffset) and the link-preview one: a saved GIF and the
// message it was saved from are two DocumentData objects for the same bytes,
// and they must not collide in Data::Session.
constexpr auto kSavedGifMediaIdOffset = qint64(3000000000000000LL);

// A GIF the server has no dimensions for. The same fallback the message
// converter uses, so a bubble and a panel cell lay out the same way.
constexpr auto kUnknownSide = 100;

// One saved GIF as the server names it: the users_gifs row, and the content
// address the file is stored under.
struct Entry {
	qint64 gifId = 0;
	QString sha256;
};

// Per-session state. A map and not a member because this adapter is a free
// namespace, like the other bridge adapters, and the state is torn down with
// the session in ClearSession().
struct State {
	// The saved-GIF list as the server last answered it, keyed by DocumentId.
	base::flat_map<DocumentId, Entry> saved;
	// Where the bytes of a document live, for every document the bridge built
	// - attachments included. "Save GIF" runs on a message's document, and the
	// server needs the sha256 of the file, which DocumentData never stores.
	base::flat_map<DocumentId, QString> sources;
	bool requesting = false;
};

base::flat_map<not_null<Main::Session*>, State> &States() {
	static auto value = base::flat_map<not_null<Main::Session*>, State>();
	return value;
}

[[nodiscard]] State &StateFor(not_null<Main::Session*> session) {
	return States()[session];
}

// Builds the document behind one panel cell.
//
// documentAttributeAnimated is what makes isGifv() true, and isGifv() is what
// GifsListWidget requires before it will lay the cell out at all
// (gifs_list_widget.cpp: createLayoutGif returns nothing without it). The video
// attribute next to it carries the geometry the mosaic packs the row with; a
// document without it claims kUnknownSide and every cell comes out square.
[[nodiscard]] DocumentData *BuildDocument(
		not_null<Main::Session*> session,
		const QJsonObject &item) {
	const auto gifId = item.value("id").toVariant().toLongLong();
	const auto url = item.value("url").toString().trimmed();
	if (gifId <= 0 || url.isEmpty()) {
		return nullptr;
	}
	const auto mediaId = kSavedGifMediaIdOffset + gifId;
	auto width = item.value("width").toInt();
	auto height = item.value("height").toInt();
	if (width <= 0 || height <= 0) {
		width = height = kUnknownSide;
	}
	const auto mime = item.value("mime").toString().isEmpty()
		? u"video/mp4"_q
		: item.value("mime").toString();
	const auto name = item.value("name").toString().isEmpty()
		? u"animation.mp4"_q
		: item.value("name").toString();
	const auto size = item.value("size").toVariant().toLongLong();

	using Flag = MTPDdocumentAttributeVideo::Flag;
	const auto attributes = QVector<MTPDocumentAttribute>{
		MTP_documentAttributeFilename(MTP_string(name)),
		MTP_documentAttributeVideo(
			MTP_flags(Flag::f_supports_streaming),
			MTP_double(0.),
			MTP_int(width),
			MTP_int(height),
			MTPint(),
			MTPdouble(),
			MTPstring()),
		MTP_documentAttributeAnimated(),
	};
	const auto document = MTP_document(
		MTP_flags(0),
		MTP_long(mediaId),
		MTP_long(0),
		MTP_bytes(),
		MTP_int(base::unixtime::now()),
		MTP_string(mime),
		MTP_long(size),
		MTPVector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(0),
		MTP_vector<MTPDocumentAttribute>(attributes));
	// Same order as the message converter: everything that says where the
	// bytes are goes on the object BEFORE processDocument() applies the
	// fields, because that call repaints whatever already shows this document
	// and the repaint is what starts the download. A document pointed at its
	// url afterwards can only build an MTProto loader, which never finishes
	// under the bridge and leaves the cell loading for good.
	const auto data = session->data().document(mediaId);
	data->setContentUrl(url);
	const auto poster = item.value("poster_url").toString().trimmed();
	if (!poster.isEmpty()) {
		data->updateThumbnails(
			InlineImageLocation(),
			ImageWithLocation{
				.location = ImageLocation(
					DownloadLocation{ PlainUrlLocation{ poster } },
					width,
					height),
			},
			ImageWithLocation(),
			false);
	}
	session->data().processDocument(document);
	return data;
}

void Apply(not_null<Main::Session*> session, const QJsonArray &items) {
	auto &state = StateFor(session);
	state.saved.clear();
	auto &saved = session->data().stickers().savedGifsRef();
	saved.clear();
	saved.reserve(items.size());
	for (const auto &value : items) {
		const auto item = value.toObject();
		const auto document = BuildDocument(session, item);
		if (!document) {
			continue;
		}
		const auto sha256 = item.value("sha256").toString().toLower();
		state.saved[document->id] = Entry{
			.gifId = item.value("id").toVariant().toLongLong(),
			.sha256 = sha256,
		};
		if (!sha256.isEmpty()) {
			state.sources[document->id] = sha256;
		}
		saved.push_back(document);
	}
	session->data().stickers().setLastSavedGifsUpdate(crl::now());
	session->data().stickers().notifySavedGifsUpdated();
}

} // namespace

void Request(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	if (state.requesting) {
		return;
	}
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return;
	}
	state.requesting = true;
	const auto weak = base::make_weak(session);
	ClientFor(session).savedGifs(0, 0, [weak](QJsonDocument doc, QString error, int) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		StateFor(strong).requesting = false;
		if (!error.isEmpty() || !doc.isObject()) {
			// The timestamp is deliberately not moved on a failure: upstream
			// re-asks after the update timeout, and pretending the list is
			// fresh would hide an empty panel behind an hour of silence.
			return;
		}
		Apply(strong, doc.object().value("items").toArray());
	});
}

void RememberSource(
		not_null<Main::Session*> session,
		DocumentId documentId,
		const QString &sha256) {
	if (documentId && !sha256.isEmpty()) {
		StateFor(session).sources[documentId] = sha256.toLower();
	}
}

QString SourceSha256(
		not_null<Main::Session*> session,
		DocumentId documentId) {
	const auto &sources = StateFor(session).sources;
	const auto i = sources.find(documentId);
	return (i == sources.end()) ? QString() : i->second;
}

void ClearSession(not_null<Main::Session*> session) {
	States().remove(session);
}

bool IsSavedGif(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document) {
	const auto &saved = StateFor(session).saved;
	return saved.find(document->id) != saved.end();
}

void Toggle(
		not_null<DocumentData*> document,
		Data::FileOrigin origin,
		bool saved) {
	const auto session = &document->session();
	// Only a message origin names a chat, and only a chat lets the server copy
	// somebody else's file. Every other origin (a sticker set, a wallpaper)
	// cannot be a GIF this product knows how to save.
	const auto message = std::get_if<Data::FileOriginMessage>(&origin.data);
	const auto peerId = message ? message->peer : PeerId();
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return;
	}
	auto &state = StateFor(session);
	if (!saved) {
		const auto i = state.saved.find(document->id);
		if (i == state.saved.end()) {
			return;
		}
		const auto gifId = i->second.gifId;
		ClientFor(session).deleteSavedGif(gifId, [weak = base::make_weak(session)](
				QJsonDocument, QString error, int) {
			const auto strong = weak.get();
			if (strong && error.isEmpty()) {
				// The response is authoritative; the list is re-read rather
				// than patched, so a concurrent change on another device does
				// not leave two clients disagreeing about the order.
				Request(strong);
			}
		});
		return;
	}
	// Every document that can be a GIF here was built by the bridge, and both
	// builders register their content address: MediaFromAttachment for one in
	// a message, Apply() for one in the panel. An id with no address is not
	// ours, and the server has no way to name the file without it.
	const auto sha256 = SourceSha256(session, document->id);
	if (sha256.isEmpty()) {
		return;
	}
	const auto history = session->data().historyLoaded(peerId);
	if (!history) {
		return;
	}
	const auto weak = base::make_weak(session);
	// The chat is what lets the server copy the file: a GIF saved from
	// somebody else's message is their file row, and it has to be duplicated
	// under the caller before it can ever be sent on.
	bridge->resolveChatId(history, [weak, sha256](qint64 chatId) {
		const auto strong = weak.get();
		if (!strong || chatId <= 0 || !BridgeFor(strong)) {
			return;
		}
		ClientFor(strong).saveGif(chatId, sha256, QString(), [weak](
				QJsonDocument, QString error, int) {
			const auto strong = weak.get();
			if (strong && error.isEmpty()) {
				Request(strong);
			}
		});
	});
}

bool Send(
		not_null<DocumentData*> document,
		const Api::SendAction &action) {
	const auto history = action.history;
	if (!history) {
		return false;
	}
	const auto session = &history->session();
	if (!IsSavedGif(session, document)) {
		return false;
	}
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return false;
	}
	// The bytes are already here: the panel had to decode them to play the
	// cell. Re-uploading them is what keeps this send on the one file path
	// that has an optimistic bubble, a retry and a cancel - and it costs
	// nothing on the server, which stores one row per content hash and hands
	// the existing object back.
	const auto media = document->createMediaView();
	auto content = media->bytes();
	auto path = document->filepath(true);
	if (content.isEmpty() && (path.isEmpty() || !QFileInfo::exists(path))) {
		// Not downloaded yet. Start it and let the user click again, which is
		// what upstream does for a document with no local copy.
		document->save(Data::FileOrigin(), QString());
		return true;
	}
	auto spec = UploadSpec{
		.path = content.isEmpty() ? path : QString(),
		.displayName = u"animation.mp4"_q,
		.mime = u"video/mp4"_q,
		.content = std::move(content),
		.kind = u"animation"_q,
	};
	auto files = std::vector<UploadSpec>();
	files.push_back(std::move(spec));
	bridge->sendFiles(
		history,
		std::move(files),
		TextWithEntities(),
		ReplyTargetFrom(history, action.replyTo),
		{},
		SendOptionsFrom(action.options));
	return true;
}

} // namespace CustomBackend::Gifs
