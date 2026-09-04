/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/file_download_web.h"

#include "custom_backend/native_runtime.h"
#include "storage/cache/storage_cache_types.h"
#include "base/timer.h"
#include "base/weak_ptr.h"

#include <QtNetwork/QAuthenticator>
#include <QtNetwork/QSslError>

namespace {

constexpr auto kMaxWebFileQueries = 8;
constexpr auto kMaxHttpRedirects = 5;
constexpr auto kResetDownloadPrioritiesTimeout = crl::time(200);
constexpr auto kMaxWebFile = 4000 * int64(1024 * 1024);

std::weak_ptr<WebLoadManager> GlobalLoadManager;

[[nodiscard]] std::shared_ptr<WebLoadManager> GetManager() {
	auto result = GlobalLoadManager.lock();
	if (!result) {
		GlobalLoadManager = result = std::make_shared<WebLoadManager>();
	}
	return result;
}

enum class ProcessResult {
	Error,
	Progress,
	Finished,
};

enum class Error {
};

struct Progress {
	qint64 ready = 0;
	qint64 total = 0;
	QByteArray streamed;
};

using Update = std::variant<Progress, QByteArray, Error>;

struct UpdateForLoader {
	not_null<webFileLoader*> loader;
	Update data;
};

// Only http(s) requests are ever performed by this loader. Otherwise a
// web content URL (or a redirect target chosen by some server) could be
// a file: URL, which on Windows opens UNC paths like \\host\share\file
// and discloses the user's NTLM credentials to that host.
[[nodiscard]] bool IsSupportedWebUrl(const QUrl &url) {
	const auto scheme = url.scheme();
	return url.isValid()
		&& (scheme == QLatin1String("http")
			|| scheme == QLatin1String("https"));
}

} // namespace

class WebLoadManager final : public base::has_weak_ptr {
public:
	WebLoadManager();
	~WebLoadManager();

	void enqueue(not_null<webFileLoader*> loader);
	void remove(not_null<webFileLoader*> loader);

	[[nodiscard]] rpl::producer<Update> updates(
		not_null<webFileLoader*> loader) const;

private:
	struct Enqueued {
		int id = 0;
		QString url;
		bool stream = false;
		// Resolved on the main thread in enqueue(): the network thread must
		// not read session state.
		CustomBackend::DownloadAuth auth;
	};
	struct Sent {
		QString url;
		not_null<QNetworkReply*> reply;
		bool stream = false;
		CustomBackend::DownloadAuth auth;
		QByteArray data;
		int64 ready = 0;
		int64 total = 0;
		bool lengthKnown = false;
		int redirectsLeft = kMaxHttpRedirects;
	};

	// Constructor.
	void handleNetworkErrors();

	// Worker thread.
	void enqueue(
		int id,
		const QString &url,
		bool stream,
		const CustomBackend::DownloadAuth &auth);
	void remove(int id);
	void resetGeneration();
	void checkSendNext();
	void send(const Enqueued &entry);
	[[nodiscard]] not_null<QNetworkReply*> send(
		int id,
		const QString &url,
		const CustomBackend::DownloadAuth &auth);
	[[nodiscard]] Sent *findSent(int id, not_null<QNetworkReply*> reply);
	void removeSent(int id);
	void progress(
		int id,
		not_null<QNetworkReply*> reply,
		int64 ready,
		int64 total);
	void failed(
		int id,
		not_null<QNetworkReply*> reply,
		QNetworkReply::NetworkError error);
	void redirect(int id, not_null<QNetworkReply*> reply);
	void notify(
		int id,
		not_null<QNetworkReply*> reply,
		int64 ready,
		int64 total);
	void failed(int id, not_null<QNetworkReply*> reply);
	void finished(int id, not_null<QNetworkReply*> reply);
	// Real end of the transfer: reads whatever the last progress callback
	// left buffered and closes the download.
	void completed(int id, not_null<QNetworkReply*> reply);
	void deleteDeferred(not_null<QNetworkReply*> reply);
	void queueProgressUpdate(
		int id,
		int64 ready,
		int64 total,
		QByteArray streamed);
	void queueFailedUpdate(int id);
	void queueFinishedUpdate(int id, const QByteArray &data);
	void clear();

	// Main thread.
	void sendUpdate(int id, Update &&data);

	QThread _thread;
	std::unique_ptr<QNetworkAccessManager> _network;
	base::Timer _resetGenerationTimer;

	// Main thread.
	rpl::event_stream<UpdateForLoader> _updates;
	int _autoincrement = 0;
	base::flat_map<not_null<webFileLoader*>, int> _ids;

	// Worker thread.
	std::deque<Enqueued> _queue;
	std::deque<Enqueued> _previousGeneration;
	base::flat_map<int, Sent> _sent;
	std::vector<QPointer<QNetworkReply>> _repliesBeingDeleted;

};

WebLoadManager::WebLoadManager()
: _network(std::make_unique<QNetworkAccessManager>())
, _resetGenerationTimer(&_thread, [=] { resetGeneration(); }) {
	// Follow redirects in Qt itself (the default in Qt 6, but not in Qt 5),
	// so that unsafe targets (scheme changes or https -> http downgrades)
	// are rejected by the HTTP layer, not re-issued by redirect() below.
	_network->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
	handleNetworkErrors();

	_network->moveToThread(&_thread);
	QObject::connect(&_thread, &QThread::finished, [=] {
		clear();
		_network = nullptr;
	});
	_thread.start();
}

void WebLoadManager::handleNetworkErrors() {
	const auto fail = [=](QNetworkReply *reply) {
		for (const auto &[id, sent] : _sent) {
			if (sent.reply == reply) {
				failed(id, reply);
				return;
			}
		}
	};
	QObject::connect(
		_network.get(),
		&QNetworkAccessManager::authenticationRequired,
		fail);
	QObject::connect(
		_network.get(),
		&QNetworkAccessManager::sslErrors,
		[=](QNetworkReply *reply, const QList<QSslError> &errors) {
			for (const auto &[id, sent] : _sent) {
				if (sent.reply == reply) {
					// Qt emits this signal even for a reply that was told to
					// ignore ssl errors (qnetworkreplyhttpimpl.cpp:1629), so
					// failing here unconditionally would kill a download the
					// bridge deliberately tolerates.
					if (!CustomBackend::AllowDownloadTls(reply, sent.auth)) {
						failed(id, reply);
					}
					return;
				}
			}
		});
}

WebLoadManager::~WebLoadManager() {
	_thread.quit();
	_thread.wait();
}

[[nodiscard]] rpl::producer<Update> WebLoadManager::updates(
		not_null<webFileLoader*> loader) const {
	return _updates.events(
	) | rpl::filter([=](const UpdateForLoader &update) {
		return (update.loader == loader);
	}) | rpl::map([=](UpdateForLoader &&update) {
		return std::move(update.data);
	});
}

void WebLoadManager::enqueue(not_null<webFileLoader*> loader) {
	const auto id = [&] {
		const auto i = _ids.find(loader);
		return (i != end(_ids))
			? i->second
			: _ids.emplace(loader, ++_autoincrement).first->second;
	}();
	const auto url = loader->url();
	const auto stream = loader->streamLoading();
	const auto auth = CustomBackend::AuthorizeDownload(
		&loader->session(),
		QUrl(url));
	InvokeQueued(_network.get(), [=] {
		enqueue(id, url, stream, auth);
	});
}

void WebLoadManager::remove(not_null<webFileLoader*> loader) {
	const auto i = _ids.find(loader);
	if (i == end(_ids)) {
		return;
	}
	const auto id = i->second;
	_ids.erase(i);
	InvokeQueued(_network.get(), [=] {
		remove(id);
	});
}

void WebLoadManager::enqueue(
		int id,
		const QString &url,
		bool stream,
		const CustomBackend::DownloadAuth &auth) {
	const auto i = ranges::find(_queue, id, &Enqueued::id);
	if (i != end(_queue)) {
		return;
	}
	if (!IsSupportedWebUrl(QUrl(url))) {
		LOG(("Network Error: "
			"Bad url requested in WebLoadManager::enqueue(): '%1'"
			).arg(url));
		queueFailedUpdate(id);
		return;
	}
	_previousGeneration.erase(
		ranges::remove(_previousGeneration, id, &Enqueued::id),
		end(_previousGeneration));
	_queue.push_back(Enqueued{ id, url, stream, auth });
	if (!_resetGenerationTimer.isActive()) {
		_resetGenerationTimer.callOnce(kResetDownloadPrioritiesTimeout);
	}
	checkSendNext();
}

void WebLoadManager::remove(int id) {
	_queue.erase(ranges::remove(_queue, id, &Enqueued::id), end(_queue));
	_previousGeneration.erase(
		ranges::remove(_previousGeneration, id, &Enqueued::id),
		end(_previousGeneration));
	removeSent(id);
}

void WebLoadManager::resetGeneration() {
	if (!_previousGeneration.empty()) {
		std::copy(
			begin(_previousGeneration),
			end(_previousGeneration),
			std::back_inserter(_queue));
		_previousGeneration.clear();
	}
	std::swap(_queue, _previousGeneration);
}

void WebLoadManager::checkSendNext() {
	if (_sent.size() >= kMaxWebFileQueries
		|| (_queue.empty() && _previousGeneration.empty())) {
		return;
	}
	const auto entry = _queue.empty()
		? _previousGeneration.front()
		: _queue.front();
	(_queue.empty() ? _previousGeneration : _queue).pop_front();
	send(entry);
}

void WebLoadManager::send(const Enqueued &entry) {
	const auto id = entry.id;
	const auto url = entry.url;
	_sent.emplace(
		id,
		Sent{ url, send(id, url, entry.auth), entry.stream, entry.auth });
}

void WebLoadManager::removeSent(int id) {
	if (const auto i = _sent.find(id); i != end(_sent)) {
		deleteDeferred(i->second.reply);
		_sent.erase(i);
		checkSendNext();
	}
}

not_null<QNetworkReply*> WebLoadManager::send(
		int id,
		const QString &url,
		const CustomBackend::DownloadAuth &auth) {
	auto request = QNetworkRequest(url);
	CustomBackend::ApplyDownloadAuth(request, auth);
	const auto result = _network->get(request);
	// Set the ignore flag before the handshake thread looks at it.
	CustomBackend::AllowDownloadTls(result, auth);
	const auto handleProgress = [=](qint64 ready, qint64 total) {
		progress(id, result, ready, total);
	};
	const auto handleError = [=](QNetworkReply::NetworkError error) {
		failed(id, result, error);
	};
	const auto handleFinished = [=] {
		completed(id, result);
	};
	QObject::connect(
		result,
		&QNetworkReply::downloadProgress,
		handleProgress);
	QObject::connect(result, &QNetworkReply::errorOccurred, handleError);
	QObject::connect(result, &QNetworkReply::finished, handleFinished);
	return result;
}

WebLoadManager::Sent *WebLoadManager::findSent(
		int id,
		not_null<QNetworkReply*> reply) {
	const auto i = _sent.find(id);
	return (i != end(_sent) && i->second.reply == reply)
		? &i->second
		: nullptr;
}

void WebLoadManager::progress(
		int id,
		not_null<QNetworkReply*> reply,
		int64 ready,
		int64 total) {
	if (total <= 0) {
		const auto originalContentLength = reply->attribute(
			QNetworkRequest::OriginalContentLengthAttribute);
		if (originalContentLength.isValid()) {
			total = originalContentLength.toLongLong();
		}
	}
	// A chunked answer carries no length, and downloadProgress then reports
	// total as the bytes seen so far. Treating that as the size makes every
	// callback look like the last one, and the download is cut at whatever
	// arrived first - silently, because the file it wrote is a valid prefix.
	// Only the reply's own finished signal ends such a transfer.
	if (const auto sent = findSent(id, reply)) {
		sent->lengthKnown = (total > 0);
	}
	const auto statusCode = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute);
	const auto status = statusCode.isValid() ? statusCode.toInt() : 200;
	if (status == 301 || status == 302) {
		redirect(id, reply);
	} else if (status != 200 && status != 206 && status != 416) {
		LOG(("Network Error: "
			"Bad HTTP status received in WebLoadManager::onProgress() %1"
			).arg(status));
		failed(id, reply);
	} else {
		notify(id, reply, ready, std::max(ready, total));
	}
}

void WebLoadManager::redirect(int id, not_null<QNetworkReply*> reply) {
	// The cooked LocationHeader value is empty for relative targets,
	// because Qt parses it into a QUrl only when it carries a scheme.
	const auto raw = reply->rawHeader("Location");
	auto url = QUrl(QString::fromUtf8(raw));
	if (!url.isValid()) {
		url = QUrl(QString::fromLatin1(raw));
	}

	if (const auto sent = findSent(id, reply)) {
		const auto next = reply->url().resolved(url);
		if (raw.isEmpty() || !IsSupportedWebUrl(next)) {
			LOG(("Network Error: "
				"Bad redirect target in WebLoadManager::redirect() "
				"for web file loader: %1").arg(QString::fromUtf8(raw)));
			failed(id, reply);
			return;
		} else if (!sent->redirectsLeft--) {
			LOG(("Network Error: "
				"Too many HTTP redirects in onFinished() "
				"for web file loader: %1").arg(next.toString()));
			failed(id, reply);
			return;
		}
		const auto target = next.toString();
		deleteDeferred(reply);
		// A redirect off our CDN must not carry the bearer with it.
		if (next.host().compare(
				QUrl(sent->url).host(),
				Qt::CaseInsensitive) != 0) {
			sent->auth = {};
		}
		sent->url = target;
		sent->reply = send(id, target, sent->auth);
	}
}

void WebLoadManager::notify(
		int id,
		not_null<QNetworkReply*> reply,
		int64 ready,
		int64 total) {
	if (const auto sent = findSent(id, reply)) {
		sent->ready = ready;
		sent->total = std::max(total, int64(0));
		if (total <= 0) {
			LOG(("Network Error: "
				"Bad size received for HTTP download progress "
				"in WebLoadManager::onProgress(): %1 / %2 (bytes %3)"
				).arg(ready
				).arg(total
				).arg(sent->data.size()));
			failed(id, reply);
			return;
		}
		auto bytes = reply->readAll();
		if (sent->stream) {
			if (total > kMaxWebFile) {
				LOG(("Network Error: "
					"Bad size received for HTTP download progress "
					"in WebLoadManager::onProgress(): %1 / %2"
					).arg(ready
					).arg(total));
				failed(id, reply);
			} else {
				queueProgressUpdate(
					id,
					sent->ready,
					sent->total,
					std::move(bytes));
				if (sent->lengthKnown && ready >= total) {
					finished(id, reply);
				}
			}
		} else {
			sent->data.append(std::move(bytes));
			if (total > Storage::kMaxFileInMemory
				|| sent->data.size() > Storage::kMaxFileInMemory) {
				LOG(("Network Error: "
					"Bad size received for HTTP download progress "
					"in WebLoadManager::onProgress(): %1 / %2 (bytes %3)"
					).arg(ready
					).arg(total
					).arg(sent->data.size()));
				failed(id, reply);
			} else if (sent->lengthKnown && ready >= total) {
				finished(id, reply);
			} else {
				queueProgressUpdate(id, sent->ready, sent->total, {});
			}
		}
	}
}

void WebLoadManager::failed(
		int id,
		not_null<QNetworkReply*> reply,
		QNetworkReply::NetworkError error) {
	if (const auto sent = findSent(id, reply)) {
		LOG(("Network Error: "
			"Failed to request '%1', error %2 (%3)"
			).arg(sent->url
			).arg(int(error)
			).arg(reply->errorString()));
		failed(id, reply);
	}
}

void WebLoadManager::failed(int id, not_null<QNetworkReply*> reply) {
	if ([[maybe_unused]] const auto sent = findSent(id, reply)) {
		removeSent(id);
		queueFailedUpdate(id);
	}
}

void WebLoadManager::deleteDeferred(not_null<QNetworkReply*> reply) {
	reply->deleteLater();
	_repliesBeingDeleted.erase(
		ranges::remove(_repliesBeingDeleted, nullptr),
		end(_repliesBeingDeleted));
	_repliesBeingDeleted.emplace_back(reply.get());
}

void WebLoadManager::completed(int id, not_null<QNetworkReply*> reply) {
	const auto sent = findSent(id, reply);
	if (!sent) {
		return;
	}
	if (reply->error() != QNetworkReply::NoError) {
		failed(id, reply);
		return;
	}
	auto bytes = reply->readAll();
	if (!bytes.isEmpty()) {
		if (sent->stream) {
			sent->ready += bytes.size();
			queueProgressUpdate(id, sent->ready, sent->ready, std::move(bytes));
		} else {
			sent->data.append(std::move(bytes));
		}
	}
	finished(id, reply);
}

void WebLoadManager::finished(int id, not_null<QNetworkReply*> reply) {
	if (const auto sent = findSent(id, reply)) {
		const auto data = base::take(sent->data);
		removeSent(id);
		queueFinishedUpdate(id, data);
	}
}

void WebLoadManager::clear() {
	for (const auto &[id, sent] : base::take(_sent)) {
		sent.reply->abort();
		delete sent.reply;
	}
	for (const auto &reply : base::take(_repliesBeingDeleted)) {
		if (reply) {
			delete reply;
		}
	}
}

void WebLoadManager::queueProgressUpdate(
		int id,
		int64 ready,
		int64 total,
		QByteArray streamed) {
	crl::on_main(this, [=, bytes = std::move(streamed)]() mutable {
		sendUpdate(id, Progress{ ready, total, std::move(bytes) });
	});
}

void WebLoadManager::queueFailedUpdate(int id) {
	crl::on_main(this, [=] {
		sendUpdate(id, Error{});
	});
}

void WebLoadManager::queueFinishedUpdate(int id, const QByteArray &data) {
	crl::on_main(this, [=] {
		for (const auto &[loader, loaderId] : _ids) {
			if (loaderId == id) {
				break;
			}
		}
		sendUpdate(id, QByteArray(data));
	});
}

void WebLoadManager::sendUpdate(int id, Update &&data) {
	for (const auto &[loader, loaderId] : _ids) {
		if (loaderId == id) {
			_updates.fire(UpdateForLoader{ loader, std::move(data) });
			return;
		}
	}
}

// A download only streams into a target file when it is too big to be held in
// memory. Everything smaller keeps the path every caller had before -
// accumulated and put in the cache, which is keyed by url, so two documents
// that happen to share one file cannot fight over a single path on disk.
// Answering LoadToCacheAsWell for everyone was what capped web downloads at
// kMaxFileInMemory, and answering LoadToFileOnly for everyone let two sends of
// the same track, which pick the same generated file name, interleave into one
// truncated file that the first finalizeResult() then registered as complete.
[[nodiscard]] QString WebLoaderTargetFile(const QString &to, int64 size) {
	return (size > Storage::kMaxFileInMemory) ? to : QString();
}

webFileLoader::webFileLoader(
	not_null<Main::Session*> session,
	const QString &url,
	const QString &to,
	int64 size,
	LoadFromCloudSetting fromCloud,
	bool autoLoading,
	uint8 cacheTag)
: FileLoader(
	session,
	WebLoaderTargetFile(to, size),
	0,
	0,
	UnknownFileLocation,
	WebLoaderTargetFile(to, size).isEmpty()
		? LoadToCacheAsWell
		: LoadToFileOnly,
	fromCloud,
	autoLoading,
	cacheTag)
, _url(url) {
}

webFileLoader::webFileLoader(
	not_null<Main::Session*> session,
	const QString &url,
	const QString &path,
	WebRequestType type)
: FileLoader(
	session,
	path,
	0,
	0,
	UnknownFileLocation,
	LoadToFileOnly,
	LoadFromCloudOrLocal,
	false,
	0)
, _url(url)
, _requestType(type) {
}

webFileLoader::~webFileLoader() {
	if (!_finished) {
		cancel();
	}
}

QString webFileLoader::url() const {
	return _url;
}

WebRequestType webFileLoader::requestType() const {
	return _requestType;
}

bool webFileLoader::streamLoading() const {
	return (_toCache == LoadToFileOnly);
}

void webFileLoader::startLoading() {
	if (_finished) {
		return;
	} else if (!_manager) {
		_manager = GetManager();
		_manager->updates(
			this
		) | rpl::on_next([=](const Update &data) {
			if (const auto progress = std::get_if<Progress>(&data)) {
				loadProgress(
					progress->ready,
					progress->total,
					progress->streamed);
			} else if (const auto bytes = std::get_if<QByteArray>(&data)) {
				loadFinished(*bytes);
			} else {
				loadFailed();
			}
		}, _managerLifetime);
	}
	_manager->enqueue(this);
}

int64 webFileLoader::currentOffset() const {
	return _ready;
}

void webFileLoader::loadProgress(
		qint64 ready,
		qint64 total,
		const QByteArray &streamed) {
	_fullSize = _loadSize = total;
	_ready = ready;
	if (!streamed.isEmpty()
		&& !writeResultPart(_streamedOffset, bytes::make_span(streamed))) {
		loadFailed();
	} else {
		_streamedOffset += streamed.size();
		notifyAboutProgress();
	}
}

void webFileLoader::loadFinished(const QByteArray &data) {
	cancelRequest();
	if (writeResultPart(0, bytes::make_span(data))) {
		finalizeResult();
	}
}

void webFileLoader::loadFailed() {
	cancel(FailureReason::OtherFailure);
}

Storage::Cache::Key webFileLoader::cacheKey() const {
	return Data::UrlCacheKey(_url);
}

std::optional<MediaKey> webFileLoader::fileLocationKey() const {
	return std::nullopt;
}

void webFileLoader::cancelHook() {
	cancelRequest();
}

void webFileLoader::cancelRequest() {
	if (!_manager) {
		return;
	}
	_managerLifetime.destroy();
	_manager->remove(this);
	_manager = nullptr;
}
