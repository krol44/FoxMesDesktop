/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_gifs_adapter.h"

#include "api/api_common.h"
#include "base/unixtime.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "custom_backend/native_streaming_loader.h"
#include "custom_backend/native_topic_channels.h"
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

constexpr auto kSavedGifMediaIdOffset = qint64(3000000000000000LL);

constexpr auto kUnknownSide = 100;

struct Entry {
	qint64 gifId = 0;
	QString sha256;
};

struct State {
	base::flat_map<DocumentId, Entry> saved;
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
	const auto data = session->data().document(mediaId);
	data->setContentUrl(url);
	Streaming::RememberSource(data, url);
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
				Request(strong);
			}
		});
		return;
	}
	const auto sha256 = SourceSha256(session, document->id);
	if (sha256.isEmpty()) {
		return;
	}
	const auto history = session->data().historyLoaded(peerId);
	if (!history) {
		return;
	}
	const auto weak = base::make_weak(session);
	const auto done = [weak](QJsonDocument, QString error, int) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		} else if (!error.isEmpty()) {
			LOG(("FoxMes Gifs: save failed: %1").arg(error));
			return;
		}
		Request(strong);
	};
	if (const auto item = message ? session->data().message(*message) : nullptr) {
		if (const auto source = TopicChannels::GifSourceOf(item)) {
			ClientFor(session).saveGif(
				source.chatId,
				sha256,
				QString(),
				done,
				source.messageId,
				source.threadRootId);
			return;
		}
	}
	bridge->resolveChatId(history, [weak, sha256, done](qint64 chatId) {
		const auto strong = weak.get();
		if (!strong || chatId <= 0 || !BridgeFor(strong)) {
			return;
		}
		ClientFor(strong).saveGif(chatId, sha256, QString(), done);
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
	const auto media = document->createMediaView();
	auto content = media->bytes();
	auto path = document->filepath(true);
	if (content.isEmpty() && (path.isEmpty() || !QFileInfo::exists(path))) {
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

}
