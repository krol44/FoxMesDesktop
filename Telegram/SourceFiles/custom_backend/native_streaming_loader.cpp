/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_streaming_loader.h"

#include "base/weak_ptr.h"
#include "custom_backend/native_runtime.h"
#include "data/data_document.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_loader.h"
#include "storage/streamed_file_downloader.h"
#include "ui/image/image_location.h"

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace CustomBackend::Streaming {
namespace {

using Media::Streaming::LoadedPart;
using Media::Streaming::SpeedEstimate;

constexpr auto kPartSize = Media::Streaming::Loader::kPartSize;

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
}

void ClearSession(not_null<Main::Session*> session) {
	States().remove(session);
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
