/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_streaming_loader.h"

#include "base/weak_ptr.h"
#include "base/openssl_help.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_runtime.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "core/file_location.h"
#include "storage/storage_account.h"
#include "storage/cache/storage_cache_database.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_loader.h"
#include "media/streaming/media_streaming_reader.h"
#include "storage/streamed_file_downloader.h"
#include "ui/image/image_location.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QUuid>
#include <map>
#include <mutex>
#include <set>

namespace CustomBackend::Streaming {
namespace {

using Media::Streaming::LoadedPart;
using Media::Streaming::SpeedEstimate;

constexpr auto kPartSize = Media::Streaming::Loader::kPartSize;
constexpr auto kMaxStreamedSize = int64(std::numeric_limits<uint32>::max());

Storage::Cache::Key LegacyUrlCacheKey(const QString &location) {
	const auto url = location.toUtf8();
	const auto hash = openssl::Sha256(bytes::make_span(url));
	const auto bytes = bytes::make_span(hash);
	const auto part1 = *reinterpret_cast<const uint32*>(bytes.data());
	const auto part2 = *reinterpret_cast<const uint64*>(bytes.data() + sizeof(uint32));
	const auto part3 = *reinterpret_cast<const uint16*>(bytes.data() + sizeof(uint32) + sizeof(uint64));
	return Storage::Cache::Key{
		0x0000030000000000ULL | (uint64(part3) << 32) | part1,
		part2,
	};
}

struct FileCacheEntry {
	Storage::Cache::Key key;
	std::set<Main::Session*> sessions;
};

struct FileCacheRegistry {
	std::mutex mutex;
	std::map<QString, FileCacheEntry> entries;
};

FileCacheRegistry &FileCacheKeys() {
	static auto value = FileCacheRegistry();
	return value;
}

constexpr auto kMaxParallelRequests = 4;

constexpr auto kMaxRetries = 2;

struct State {
	base::flat_map<DocumentId, QString> urls;
	base::flat_map<DocumentId, QString> playlists;
	std::shared_ptr<QNetworkAccessManager> manager;
};

[[nodiscard]] base::flat_map<not_null<Main::Session*>, State> &States() {
	static auto value = base::flat_map<not_null<Main::Session*>, State>();
	return value;
}

[[nodiscard]] std::shared_ptr<QNetworkAccessManager> ManagerFor(
		not_null<Main::Session*> session) {
	[[maybe_unused]] static const auto release = [] {
		ReleaseOnQuit([] { States().clear(); });
		return true;
	}();
	auto &manager = States()[session].manager;
	if (!manager) {
		manager = std::make_shared<QNetworkAccessManager>();
		manager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
	}
	return manager;
}

[[nodiscard]] Storage::Cache::Key RangeCacheKey(Storage::Cache::Key key) {
	return Storage::Cache::Key{
		0x0000050000000000ULL | (key.high & 0x000000FFFFFFFFFFULL),
		key.low & ~0xFFFFULL,
	};
}

[[nodiscard]] QString SourceUrl(not_null<const DocumentData*> document) {
	const auto session = &document->session();
	const auto i = States().find(session);
	if (i == end(States())) {
		return QString();
	}
	const auto j = i->second.urls.find(document->id);
	return (j == end(i->second.urls)) ? QString() : j->second;
}

[[nodiscard]] QString PlaylistOf(not_null<const DocumentData*> document) {
	const auto i = States().find(&document->session());
	if (i == end(States())) {
		return QString();
	}
	const auto j = i->second.playlists.find(document->id);
	return (j == end(i->second.playlists)) ? QString() : j->second;
}

class Loader final
	: public Media::Streaming::Loader
	, public base::has_weak_ptr {
public:
	Loader(
		not_null<Main::Session*> session,
		std::shared_ptr<QNetworkAccessManager> manager,
		const QString &url,
		Storage::Cache::Key baseKey,
		int64 size,
		const QString &playlist = QString());
	~Loader();

	[[nodiscard]] Storage::Cache::Key baseCacheKey() const override;
	[[nodiscard]] int64 size() const override;
	[[nodiscard]] bool mpegTsStream() const override;
	[[nodiscard]] std::optional<int64> mpegTsSegmentOffset(
		crl::time position) const override;

	void load(int64 offset) override;
	void cancel(int64 offset) override;
	void resetPriorities() override;
	void setPriority(int priority) override;
	void stop() override;

	void tryRemoveFromQueue() override;

	[[nodiscard]] rpl::producer<LoadedPart> parts() const override;
	[[nodiscard]] rpl::producer<SpeedEstimate> speedEstimate() const override;

	void attachDownloader(
		not_null<Storage::StreamedFileDownloader*> downloader) override;
	void clearAttachedDownloader() override;

private:
	struct Piece {
		QString url;
		int64 from = 0;
		int64 till = 0;
		int64 total = 0;
	};
	struct Request {
		QPointer<QNetworkReply> reply;
		int retries = 0;
		std::vector<Piece> pieces;
		int piece = 0;
		QByteArray bytes;
	};
	struct Segment {
		QString url;
		int64 offset = 0;
		int64 size = 0;
	};
	enum class Segments : uchar {
		Unknown,
		Loading,
		Ready,
		Failed,
	};

	void loadOnMain(int64 offset);
	void cancelOnMain(int64 offset);
	void sendNext();
	void send(int64 offset);
	void sendPiece(int64 offset);
	void finished(int64 offset, not_null<QNetworkReply*> reply);
	void failed(int64 offset);
	[[nodiscard]] int64 partSizeAt(int64 offset) const;
	[[nodiscard]] std::vector<Piece> piecesAt(int64 offset) const;
	void abort(Request &request);
	void requestSegments();
	void segmentsLoaded(const QJsonDocument &doc, const QString &error);

	const not_null<Main::Session*> _session;
	const QString _url;
	const Storage::Cache::Key _baseKey;
	const int64 _size = 0;
	const std::shared_ptr<QNetworkAccessManager> _manager;
	const QString _playlist;
	Segments _segmentsState = Segments::Unknown;
	std::vector<Segment> _segments;
	mutable std::mutex _seekMutex;
	std::vector<std::pair<crl::time, int64>> _seekPoints;

	Media::Streaming::PriorityQueue _queued;
	base::flat_map<int64, Request> _sent;
	rpl::event_stream<LoadedPart> _parts;
	Storage::StreamedFileDownloader *_downloader = nullptr;

};

Loader::Loader(
	not_null<Main::Session*> session,
	std::shared_ptr<QNetworkAccessManager> manager,
	const QString &url,
	Storage::Cache::Key baseKey,
	int64 size,
	const QString &playlist)
: _session(session)
, _url(url)
, _baseKey(baseKey)
, _size(size)
, _manager(std::move(manager))
, _playlist(playlist)
, _segmentsState(playlist.isEmpty() ? Segments::Ready : Segments::Unknown) {
	Expects(size > 0);
	Expects(_manager != nullptr);

	if (!_playlist.isEmpty()) {
		crl::on_main(this, [=] {
			requestSegments();
		});
	}
}

Loader::~Loader() {
	for (auto &[offset, request] : _sent) {
		abort(request);
	}
}

Storage::Cache::Key Loader::baseCacheKey() const {
	return _baseKey;
}

int64 Loader::size() const {
	return _size;
}

bool Loader::mpegTsStream() const {
	return !_playlist.isEmpty();
}

std::optional<int64> Loader::mpegTsSegmentOffset(crl::time position) const {
	const auto lock = std::lock_guard(_seekMutex);
	if (_seekPoints.empty()) {
		return std::nullopt;
	}
	const auto i = std::upper_bound(
		begin(_seekPoints),
		end(_seekPoints),
		position,
		[](crl::time position, const std::pair<crl::time, int64> &point) {
			return position < point.first;
		});
	return (i == begin(_seekPoints)) ? int64(0) : std::prev(i)->second;
}

int64 Loader::partSizeAt(int64 offset) const {
	return std::min(kPartSize, _size - offset);
}

auto Loader::piecesAt(int64 offset) const -> std::vector<Piece> {
	const auto till = offset + partSizeAt(offset);
	if (_playlist.isEmpty()) {
		return { Piece{ _url, offset, till, _size } };
	}
	auto result = std::vector<Piece>();
	auto i = std::upper_bound(
		begin(_segments),
		end(_segments),
		offset,
		[](int64 offset, const Segment &segment) {
			return offset < segment.offset;
		});
	Assert(i != begin(_segments));
	for (--i; offset < till && i != end(_segments); ++i) {
		const auto pieceTill = std::min(till, i->offset + i->size);
		result.push_back({
			i->url,
			offset - i->offset,
			pieceTill - i->offset,
			i->size,
		});
		offset = pieceTill;
	}
	return result;
}

void Loader::requestSegments() {
	if (_segmentsState != Segments::Unknown) {
		return;
	}
	_segmentsState = Segments::Loading;
	ClientFor(_session).hlsSegments(_playlist, crl::guard(this, [=](
			QJsonDocument doc,
			QString error,
			int) {
		segmentsLoaded(doc, error);
	}));
}

void Loader::segmentsLoaded(const QJsonDocument &doc, const QString &error) {
	auto segments = std::vector<Segment>();
	auto seekPoints = std::vector<std::pair<crl::time, int64>>();
	auto offset = int64();
	auto time = crl::time();
	auto timed = true;
	for (const auto &entry : doc.object().value("segments").toArray()) {
		const auto segment = entry.toObject();
		const auto url = segment.value("url").toString();
		const auto size = segment.value("size").toVariant().toLongLong();
		const auto duration = crl::time(
			segment.value("duration_ms").toVariant().toLongLong());
		if (url.isEmpty() || size <= 0) {
			segments.clear();
			break;
		}
		segments.push_back({ url, offset, size });
		seekPoints.emplace_back(time, offset);
		offset += size;
		time += duration;
		timed = timed && (duration > 0);
	}
	if (!timed) {
		seekPoints.clear();
	}
	if (!error.isEmpty() || segments.empty() || offset != _size) {
		LOG(("FoxMes Streaming: segments of playlist %1 failed "
			"(error '%2', %3 bytes of %4)"
			).arg(_playlist, error).arg(offset).arg(_size));
		if (_queued.empty()) {
			_segmentsState = Segments::Unknown;
			return;
		}
		_segmentsState = Segments::Failed;
		_queued.clear();
		failed(0);
		return;
	}
	_segments = std::move(segments);
	_segmentsState = Segments::Ready;
	{
		const auto lock = std::lock_guard(_seekMutex);
		_seekPoints = std::move(seekPoints);
	}
	sendNext();
}

void Loader::load(int64 offset) {
	crl::on_main(this, [=] {
		loadOnMain(offset);
	});
}

void Loader::loadOnMain(int64 offset) {
	if (_sent.contains(offset)) {
		return;
	} else if (_downloader) {
		auto bytes = _downloader->readLoadedPart(offset);
		if (!bytes.isEmpty()) {
			_queued.remove(offset);
			_parts.fire({ offset, std::move(bytes) });
			return;
		}
	}
	if (_segmentsState == Segments::Failed) {
		failed(offset);
		return;
	}
	_queued.add(offset);
	if (_segmentsState != Segments::Ready) {
		requestSegments();
		return;
	}
	sendNext();
}

void Loader::cancel(int64 offset) {
	crl::on_main(this, [=] {
		cancelOnMain(offset);
	});
}

void Loader::cancelOnMain(int64 offset) {
	_queued.remove(offset);
	const auto i = _sent.find(offset);
	if (i != end(_sent)) {
		abort(i->second);
		_sent.erase(i);
		sendNext();
	}
}

void Loader::resetPriorities() {
	crl::on_main(this, [=] {
		_queued.resetPriorities();
	});
}

void Loader::setPriority(int priority) {
}

void Loader::stop() {
	crl::on_main(this, [=] {
		_queued.clear();
		for (auto &[offset, request] : _sent) {
			abort(request);
		}
		_sent.clear();
	});
}

void Loader::tryRemoveFromQueue() {
}

void Loader::abort(Request &request) {
	if (const auto reply = request.reply.data()) {
		request.reply = nullptr;
		reply->disconnect();
		reply->abort();
		reply->deleteLater();
	}
}

void Loader::sendNext() {
	if (_segmentsState != Segments::Ready) {
		return;
	}
	while (int(_sent.size()) < kMaxParallelRequests) {
		const auto offset = _queued.take();
		if (!offset) {
			return;
		}
		send(*offset);
	}
}

void Loader::send(int64 offset) {
	_sent[offset] = Request{ .pieces = piecesAt(offset) };
	sendPiece(offset);
}

void Loader::sendPiece(int64 offset) {
	auto &state = _sent[offset];
	const auto &piece = state.pieces[state.piece];
	auto request = QNetworkRequest(QUrl(piece.url));
	request.setRawHeader(
		"Range",
		"bytes="
			+ QByteArray::number(piece.from)
			+ "-"
			+ QByteArray::number(piece.till - 1));
	const auto auth = AuthorizeDownload(_session, request.url());
	ApplyDownloadAuth(request, auth);
	const auto reply = _manager->get(request);
	AllowDownloadTls(reply, auth);

	state.reply = reply;
	QObject::connect(reply, &QNetworkReply::finished, reply, crl::guard(this, [=] {
		finished(offset, reply);
	}));
}

void Loader::finished(int64 offset, not_null<QNetworkReply*> reply) {
	const auto i = _sent.find(offset);
	if (i == end(_sent) || i->second.reply.data() != reply.get()) {
		return;
	}
	auto &state = i->second;
	state.reply = nullptr;
	const auto piece = state.pieces[state.piece];

	const auto error = reply->error();
	const auto status = reply
		->attribute(QNetworkRequest::HttpStatusCodeAttribute)
		.toInt();
	const auto expected = piece.till - piece.from;
	auto bytes = reply->readAll();
	reply->deleteLater();

	if (error != QNetworkReply::NoError) {
		if (state.retries < kMaxRetries) {
			++state.retries;
			sendPiece(offset);
			return;
		}
		_sent.erase(i);
		failed(offset);
		return;
	} else if (status == 200 && int64(bytes.size()) == piece.total) {
		bytes = bytes.mid(piece.from, expected);
	} else if (status != 206 || int64(bytes.size()) != expected) {
		_sent.erase(i);
		failed(offset);
		return;
	}
	if (int64(bytes.size()) != expected) {
		_sent.erase(i);
		failed(offset);
		return;
	}
	state.bytes.append(bytes);
	if (++state.piece < int(state.pieces.size())) {
		sendPiece(offset);
		return;
	}
	bytes = std::move(state.bytes);
	_sent.erase(i);
	const auto weak = base::make_weak(this);
	_parts.fire({ offset, std::move(bytes) });
	if (weak) {
		sendNext();
	}
}

void Loader::failed(int64 offset) {
	const auto weak = base::make_weak(this);
	_parts.fire({ LoadedPart::kFailedOffset });
	if (weak) {
		sendNext();
	}
}

rpl::producer<LoadedPart> Loader::parts() const {
	return _parts.events();
}

rpl::producer<SpeedEstimate> Loader::speedEstimate() const {
	return rpl::never<SpeedEstimate>();
}

void Loader::attachDownloader(
		not_null<Storage::StreamedFileDownloader*> downloader) {
	_downloader = downloader;
}

void Loader::clearAttachedDownloader() {
	_downloader = nullptr;
}

constexpr auto kMpegTsSeekStep = crl::time(2000);
constexpr auto kMpegTsSeekAttempts = 6;
constexpr auto kMpegTsKeyframeSearchPackets = 4096;

[[nodiscard]] int64 MpegTsStart(
		not_null<AVFormatContext*> format,
		not_null<AVStream*> stream) {
	return (format->start_time != AV_NOPTS_VALUE && format->start_time > 0)
		? av_rescale_q(format->start_time, AV_TIME_BASE_Q, stream->time_base)
		: 0;
}

[[nodiscard]] std::optional<std::pair<int64, int64>> NextMpegTsKeyframe(
		not_null<AVFormatContext*> format,
		int streamIndex,
		int &error) {
	for (auto i = 0; i != kMpegTsKeyframeSearchPackets; ++i) {
		auto packet = FFmpeg::Packet();
		auto &fields = packet.fields();
		if ((error = av_read_frame(format, &fields)) < 0) {
			if (error == AVERROR_EOF) {
				error = 0;
			}
			return std::nullopt;
		}
		const auto time = (fields.pts != AV_NOPTS_VALUE)
			? fields.pts
			: fields.dts;
		if (fields.stream_index == streamIndex
			&& (fields.flags & AV_PKT_FLAG_KEY)
			&& fields.pos >= 0
			&& time != AV_NOPTS_VALUE) {
			return std::make_pair(int64(fields.pos), int64(time));
		}
	}
	return std::nullopt;
}

} // namespace

void ShiftMpegTsPacket(not_null<AVFormatContext*> format, AVPacket &packet) {
	if (packet.stream_index < 0
		|| packet.stream_index >= int(format->nb_streams)) {
		return;
	}
	const auto start = MpegTsStart(format, format->streams[packet.stream_index]);
	if (packet.pts != AV_NOPTS_VALUE) {
		packet.pts -= start;
	}
	if (packet.dts != AV_NOPTS_VALUE) {
		packet.dts -= start;
	}
}

int SeekMpegTs(
		not_null<AVFormatContext*> format,
		int streamIndex,
		crl::time position,
		not_null<const Media::Streaming::Reader*> reader) {
	const auto stream = format->streams[streamIndex];
	const auto start = MpegTsStart(format, stream);
	const auto ms = AVRational{ 1, 1000 };
	const auto target = start + av_rescale_q(position, ms, stream->time_base);

	const auto first = (stream->start_time != AV_NOPTS_VALUE)
		? stream->start_time
		: start;
	const auto fromFirst = av_rescale_q(target - first, stream->time_base, ms);
	if (const auto offset = reader->mpegTsSegmentOffset(fromFirst)) {
		return av_seek_frame(format, -1, *offset, AVSEEK_FLAG_BYTE);
	}
	auto back = int64(0);
	for (auto i = 0; i != kMpegTsSeekAttempts; ++i) {
		const auto from = target - back;
		if (from <= start) {
			break;
		}
		auto error = av_seek_frame(format, streamIndex, from, AVSEEK_FLAG_BACKWARD);
		if (error < 0) {
			return error;
		}
		const auto keyframe = NextMpegTsKeyframe(format, streamIndex, error);
		if (error < 0) {
			return error;
		} else if (keyframe && keyframe->second <= target) {
			return av_seek_frame(format, -1, keyframe->first, AVSEEK_FLAG_BYTE);
		}
		back = back
			? (back * 2)
			: av_rescale_q(kMpegTsSeekStep, ms, stream->time_base);
	}
	return av_seek_frame(format, -1, 0, AVSEEK_FLAG_BYTE);
}

void RememberSource(not_null<DocumentData*> document, const QString &url) {
	if (url.isEmpty()) {
		return;
	}
	States()[&document->session()].urls[document->id] = url;
	if (const auto key = FileLocationKey(url)) {
		if (!document->location().check()) {
			document->setLocation(document->session().local().readFileLocation(*key));
		}
	}
}

void RememberPlaylist(
		not_null<DocumentData*> document,
		const QString &playlistSha256) {
	if (!playlistSha256.isEmpty()) {
		States()[&document->session()].playlists[document->id] = playlistSha256;
	}
}

void RememberFileCacheKey(
		not_null<Main::Session*> session,
		const QString &url,
		const QString &fileUniqueId,
		const QString &representation) {
	const auto uuid = QUuid(fileUniqueId);
	if (url.isEmpty() || uuid.isNull()) {
		return;
	}
	const auto identity = u"foxmes-file-v1:"_q
		+ uuid.toString(QUuid::WithoutBraces) + ':' + representation;
	const auto key = LegacyUrlCacheKey(identity);
	const auto previous = UrlCacheKey(url);
	auto &registry = FileCacheKeys();
	{
		const auto lock = std::lock_guard(registry.mutex);
		auto &entry = registry.entries[url];
		entry.key = key;
		if (!entry.sessions.insert(session.get()).second) {
			return;
		}
	}
	if (previous != key) {
		session->data().cache().copyIfEmpty(previous, key);
	}
}

std::optional<Storage::Cache::Key> FileCacheKey(const QString &url) {
	auto &registry = FileCacheKeys();
	const auto lock = std::lock_guard(registry.mutex);
	const auto i = registry.entries.find(url);
	return (i == registry.entries.end())
		? std::nullopt : std::make_optional(i->second.key);
}

Storage::Cache::Key UrlCacheKey(const QString &url) {
	if (const auto key = FileCacheKey(url)) {
		return *key;
	}
	return LegacyUrlCacheKey(url);
}

std::optional<MediaKey> FileLocationKey(const QString &url) {
	if (const auto key = FileCacheKey(url)) {
		return MediaKey{ key->high, key->low };
	}
	return std::nullopt;
}

MediaKey DocumentMediaKey(
		const QString &url,
		LocationType type,
		int32 dc,
		uint64 id) {
	if (const auto key = FileCacheKey(url)) {
		return { key->high, key->low };
	}
	return ::mediaKey(type, dc, id);
}

void ClearSession(not_null<Main::Session*> session) {
	States().remove(session);
	auto &registry = FileCacheKeys();
	const auto lock = std::lock_guard(registry.mutex);
	for (auto i = registry.entries.begin(); i != registry.entries.end();) {
		i->second.sessions.erase(session.get());
		if (i->second.sessions.empty()) {
			i = registry.entries.erase(i);
		} else {
			++i;
		}
	}
}

bool CanBeStreamed(not_null<const DocumentData*> document) {
	return document->supportsStreaming()
		&& (document->size > 0)
		&& (document->size <= kMaxStreamedSize)
		&& !SourceUrl(document).isEmpty();
}

Storage::Cache::Key BigFileCacheKey(not_null<const DocumentData*> document) {
	if (SourceUrl(document).isEmpty()) {
		return Storage::Cache::Key();
	}
	if (const auto key = FileCacheKey(SourceUrl(document))) {
		return RangeCacheKey(*key);
	}
	return StorageFileLocation(
		0,
		document->session().userId(),
		MTP_inputDocumentFileLocation(
			MTP_long(document->id),
			MTP_long(0),
			MTP_bytes(),
			MTP_string())).bigFileBaseCacheKey();
}

std::unique_ptr<Media::Streaming::Loader> MakeLoader(
		not_null<const DocumentData*> document) {
	if (!CanBeStreamed(document)) {
		return nullptr;
	}
	const auto url = SourceUrl(document);
	if (url.isEmpty()) {
		return nullptr;
	}
	const auto session = &document->session();
	return std::make_unique<Loader>(
		session,
		ManagerFor(session),
		url,
		BigFileCacheKey(document),
		document->size,
		PlaylistOf(document));
}

std::unique_ptr<Media::Streaming::Loader> MakePhotoVideoLoader(
		not_null<const PhotoData*> photo) {
	if (!photo->hasVideo()) {
		return nullptr;
	}
	constexpr auto large = Data::PhotoSize::Large;
	const auto plain = std::get_if<PlainUrlLocation>(
		&photo->videoLocation(large).file().data);
	const auto size = photo->videoByteSize(large);
	if (!plain
		|| plain->url.isEmpty()
		|| size <= 0
		|| size > kMaxStreamedSize) {
		return nullptr;
	}
	const auto session = &photo->session();
	return std::make_unique<Loader>(
		session,
		ManagerFor(session),
		plain->url,
		RangeCacheKey(UrlCacheKey(plain->url)),
		size);
}

}
