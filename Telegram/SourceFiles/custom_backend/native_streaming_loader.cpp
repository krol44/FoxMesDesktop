/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_streaming_loader.h"

#include "base/weak_ptr.h"
#include "base/openssl_help.h"
#include "custom_backend/native_runtime.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "core/file_location.h"
#include "storage/storage_account.h"
#include "storage/cache/storage_cache_database.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_loader.h"
#include "storage/streamed_file_downloader.h"
#include "ui/image/image_location.h"

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

// Keep the legacy URL layout compatible with Data::UrlCacheKey for resources without a UUID.
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

// URL cache keys are also read by download workers; session state below is main-thread only.
struct FileCacheRegistry {
	std::mutex mutex;
	std::map<QString, FileCacheEntry> entries;
};

FileCacheRegistry &FileCacheKeys() {
	static auto value = FileCacheRegistry();
	return value;
}

// How many part requests are in flight at once. Upstream lets the MTProto
// download manager decide; over HTTP the number is ours, and four is what fills
// the pipe without turning one playback into a burst the CDN has to fan out.
constexpr auto kMaxParallelRequests = 4;

// A part is retried this many times before the stream is failed. Only transport
// errors are retried - an HTTP status is an answer, and repeating a 403 would
// just be a slower 403.
constexpr auto kMaxRetries = 2;

// Where the bytes of every document the bridge built live. DocumentData keeps
// the content url privately and hands out no getter, so this map is both the
// url source and the test for "is this one of ours".
struct State {
	base::flat_map<DocumentId, QString> urls;
	// One manager per session, so the parts of every video reuse the same
	// pooled connections instead of opening a TLS handshake per loader. Held
	// by shared_ptr because a loader outliving ClearSession() must not be left
	// with a dangling reference.
	std::shared_ptr<QNetworkAccessManager> manager;
};

[[nodiscard]] base::flat_map<not_null<Main::Session*>, State> &States() {
	static auto value = base::flat_map<not_null<Main::Session*>, State>();
	return value;
}

[[nodiscard]] std::shared_ptr<QNetworkAccessManager> ManagerFor(
		not_null<Main::Session*> session) {
	// A manager left in States() when the static destructors run would join
	// its worker thread with no QCoreApplication left to shut it down, and
	// exit() would never return.
	[[maybe_unused]] static const auto release = [] {
		ReleaseOnQuit([] { States().clear(); });
		return true;
	}();
	auto &manager = States()[session].manager;
	if (!manager) {
		manager = std::make_shared<QNetworkAccessManager>();
		// Qt rejects an unsafe redirect itself, so a moved attachment cannot
		// be used to downgrade the request to http and leak the bearer.
		manager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
	}
	return manager;
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

class Loader final
	: public Media::Streaming::Loader
	, public base::has_weak_ptr {
public:
	Loader(
		not_null<Main::Session*> session,
		std::shared_ptr<QNetworkAccessManager> manager,
		const QString &url,
		Storage::Cache::Key baseKey,
		int64 size);
	~Loader();

	[[nodiscard]] Storage::Cache::Key baseCacheKey() const override;
	[[nodiscard]] int64 size() const override;

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
	struct Request {
		QPointer<QNetworkReply> reply;
		int retries = 0;
	};

	// Everything below runs on the main thread only: Reader calls the public
	// methods from its own thread, and each of them hops here first.
	void loadOnMain(int64 offset);
	void cancelOnMain(int64 offset);
	void sendNext();
	void send(int64 offset, int retries);
	void finished(int64 offset, not_null<QNetworkReply*> reply);
	void failed(int64 offset);
	[[nodiscard]] int64 partSizeAt(int64 offset) const;
	void abort(Request &request);

	const not_null<Main::Session*> _session;
	const QString _url;
	const Storage::Cache::Key _baseKey;
	const int64 _size = 0;
	const std::shared_ptr<QNetworkAccessManager> _manager;

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
	int64 size)
: _session(session)
, _url(url)
, _baseKey(baseKey)
, _size(size)
, _manager(std::move(manager)) {
	Expects(size > 0);
	Expects(_manager != nullptr);
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

int64 Loader::partSizeAt(int64 offset) const {
	return std::min(kPartSize, _size - offset);
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
		// The same part may already be on disk under the file downloader that
		// drives a "save as": upstream reads it from there instead of asking
		// the network twice.
		auto bytes = _downloader->readLoadedPart(offset);
		if (!bytes.isEmpty()) {
			_queued.remove(offset);
			_parts.fire({ offset, std::move(bytes) });
			return;
		}
	}
	_queued.add(offset);
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
	while (int(_sent.size()) < kMaxParallelRequests) {
		const auto offset = _queued.take();
		if (!offset) {
			return;
		}
		send(*offset, 0);
	}
}

void Loader::send(int64 offset, int retries) {
	const auto till = offset + partSizeAt(offset);
	auto request = QNetworkRequest(QUrl(_url));
	request.setRawHeader(
		"Range",
		"bytes="
			+ QByteArray::number(offset)
			+ "-"
			+ QByteArray::number(till - 1));
	const auto auth = AuthorizeDownload(_session, request.url());
	ApplyDownloadAuth(request, auth);
	const auto reply = _manager->get(request);
	// Arms the reply before the handshake, exactly as the web file loader
	// does; false and a no-op outside a dev build.
	AllowDownloadTls(reply, auth);

	_sent[offset] = Request{ .reply = reply, .retries = retries };
	// Guarded, not just bound to the reply: the connection outlives this loader
	// whenever a reply is answered after the loader is gone, and the handler
	// touches nothing but the loader.
	QObject::connect(reply, &QNetworkReply::finished, reply, crl::guard(this, [=] {
		finished(offset, reply);
	}));
}

void Loader::finished(int64 offset, not_null<QNetworkReply*> reply) {
	const auto i = _sent.find(offset);
	if (i == end(_sent) || i->second.reply.data() != reply.get()) {
		// Cancelled or replaced while the answer was on its way.
		return;
	}
	const auto retries = i->second.retries;
	i->second.reply = nullptr;
	_sent.erase(i);

	const auto error = reply->error();
	const auto status = reply
		->attribute(QNetworkRequest::HttpStatusCodeAttribute)
		.toInt();
	const auto expected = partSizeAt(offset);
	auto bytes = reply->readAll();
	reply->deleteLater();

	if (error != QNetworkReply::NoError) {
		if (retries < kMaxRetries) {
			send(offset, retries + 1);
			return;
		}
		failed(offset);
		return;
	} else if (status == 200 && int64(bytes.size()) == _size) {
		// The server answered the whole file instead of the range. Wasteful,
		// but the bytes are right there, so the part is served from them
		// rather than failing a playback over it.
		bytes = bytes.mid(offset, expected);
	} else if (status != 206 || int64(bytes.size()) != expected) {
		failed(offset);
		return;
	}
	if (int64(bytes.size()) != expected) {
		failed(offset);
		return;
	}
	// Firing a part can destroy this loader synchronously: the Reader owns the
	// loader and is itself destroyed from inside the fire when the part
	// completes a download somebody is waiting on - deleting the message being
	// played is one way there. Upstream says as much where it consumes the
	// stream (media_streaming_reader.cpp). So nothing may touch the loader
	// after a fire without proving it is still alive.
	const auto weak = base::make_weak(this);
	_parts.fire({ offset, std::move(bytes) });
	if (weak) {
		sendNext();
	}
}

void Loader::failed(int64 offset) {
	// Same re-entrancy as in finished(): a failed part tears the playback down,
	// and the loader can be gone by the time the fire returns.
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

} // namespace

void RememberSource(not_null<DocumentData*> document, const QString &url) {
	if (url.isEmpty()) {
		return;
	}
	States()[&document->session()].urls[document->id] = url;
	if (const auto key = FileLocationKey(url)) {
		// HTTP documents keep dc/access at zero, so setRemoteLocation() never restores their path.
		if (!document->location().check()) {
			document->setLocation(document->session().local().readFileLocation(*key));
		}
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
	// supportsStreaming() is the documentAttributeVideo flag, the same gate
	// upstream applies on top of its own location check.
	return document->supportsStreaming()
		&& (document->size > 0)
		&& !SourceUrl(document).isEmpty();
}

Storage::Cache::Key BigFileCacheKey(not_null<const DocumentData*> document) {
	if (SourceUrl(document).isEmpty()) {
		return Storage::Cache::Key();
	}
	if (const auto key = FileCacheKey(SourceUrl(document))) {
		// A separate namespace for ranges, with low bits reserved for Reader's slice number.
		return Storage::Cache::Key{
			0x0000050000000000ULL | (key->high & 0x000000FFFFFFFFFFULL),
			key->low & ~0xFFFFULL,
		};
	}
	// The layout upstream computes for a document, with the dc id left at
	// zero: our attachments have none, and upstream never issues dc 0, so a
	// key of ours cannot collide with a real MTProto document. Built through
	// StorageFileLocation instead of by hand so it stays that same layout.
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
		document->size);
}

} // namespace CustomBackend::Streaming
