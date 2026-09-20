/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/github_update.h"

#include "base/algorithm.h"
#include "base/debug_log.h"
#include "base/platform/base_platform_file_utilities.h"
#include "base/timer.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/version.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_runtime.h"
#include "ui/toast/toast.h"
#include "storage/localstorage.h"
#include "lang/lang_keys.h"

#include <ksandbox.h>

#include <gsl/gsl>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif // Q_OS_WIN

#include <cmath>
#include <limits>
#include <optional>

namespace CustomBackend::Updates {
namespace {

const auto kManifestUrl = u"https://github.com/krol44/FoxMesDesktop/"_q
	+ u"releases/latest/download/version.json"_q;
const auto kReleasePageUrl = u"https://github.com/krol44/FoxMesDesktop/"_q
	+ u"releases/latest"_q;

constexpr auto kRequestTimeoutMs = 30 * 1000;
// A release package is tens to hundreds of megabytes on a link we do not
// control, so it gets its own budget rather than the manifest's.
constexpr auto kPackageTimeoutMs = 30 * 60 * 1000;
constexpr auto kMaxManifestSize = 16 * 1024;
constexpr auto kMaxPackageSize = qint64(512) * 1024 * 1024;
constexpr auto kHashChunkSize = 1024 * 1024;
constexpr auto kRecheckInterval = 60 * 60 * crl::time(1000);

struct PackageAsset {
	QString url;
	QString sha256;

	[[nodiscard]] bool valid() const {
		return !url.isEmpty() && !sha256.isEmpty();
	}
};

struct ManifestInfo {
	qint64 versionCode = 0;
	QString version;
	// Absent when the manifest carries no block for this platform: the client
	// then keeps the old notify-only behaviour instead of failing the check.
	PackageAsset asset;
};

struct ReadyPackage {
	qint64 versionCode = 0;
	QString version;
	QString path;
};

[[nodiscard]] bool DevOverrideActive() {
#if FOXMES_ALLOW_ENDPOINT_OVERRIDE
	return std::getenv("FOXMES_UPDATE_MANIFEST_URL") != nullptr;
#else // FOXMES_ALLOW_ENDPOINT_OVERRIDE
	return false;
#endif // !FOXMES_ALLOW_ENDPOINT_OVERRIDE
}

[[nodiscard]] QString ManifestUrl() {
#if FOXMES_ALLOW_ENDPOINT_OVERRIDE
	// Dev-only, exactly like FOXMES_URL in native_runtime: release packaging
	// builds with the option OFF and never reads the variable.
	if (const auto env = std::getenv("FOXMES_UPDATE_MANIFEST_URL")) {
		const auto value = QString::fromUtf8(env).trimmed();
		if (!value.isEmpty()) {
			return value;
		}
	}
#endif // FOXMES_ALLOW_ENDPOINT_OVERRIDE
	return kManifestUrl;
}

[[nodiscard]] QString ReleasePageUrl() {
	return kReleasePageUrl;
}

[[nodiscard]] bool IsAllowedManifestUrl(const QUrl &url) {
	if (DevOverrideActive()) {
		return true;
	}
	const auto host = url.host().toLower();
	return (url.scheme() == u"https"_q)
		&& (url.port(-1) == -1 || url.port() == 443)
		&& url.userInfo().isEmpty()
		&& ((host == u"github.com"_q)
			|| host.endsWith(u".githubusercontent.com"_q));
}

// The manifest block this build installs from. Linux self-update covers the
// AppImage only: a deb/rpm/portable tree belongs to whoever installed it, and
// a sandboxed build must go through its store, exactly as upstream does for
// flatpak and snap.
[[nodiscard]] QString PlatformKey() {
#ifdef Q_OS_WIN
	return u"windows"_q;
#elif defined Q_OS_MAC // Q_OS_WIN
	return u"macos"_q;
#else // Q_OS_MAC
	if (KSandbox::isFlatpak() || KSandbox::isSnap()) {
		return QString();
	} else if (qEnvironmentVariableIsEmpty("APPIMAGE")) {
		return QString();
	}
	return u"linux_appimage"_q;
#endif // else for Q_OS_WIN || Q_OS_MAC
}

[[nodiscard]] QString UpdatesFolder() {
	return cWorkingDir() + u"fox_updates/"_q;
}

[[nodiscard]] QString SidecarPath() {
	return UpdatesFolder() + u"ready.json"_q;
}

[[nodiscard]] QString Sha256OfFile(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	while (!file.atEnd()) {
		const auto chunk = file.read(kHashChunkSize);
		if (chunk.isEmpty()) {
			return QString();
		}
		hash.addData(chunk);
	}
	return QString::fromLatin1(hash.result().toHex());
}

void ClearFolder() {
	QDir(UpdatesFolder()).removeRecursively();
}

} // namespace

namespace {

// Downloads one release asset, verifies its SHA-256 and keeps the verified
// file on disk. A digest mismatch is terminal for that package: the partial
// file is dropped and nothing is ever handed to an installer unverified.
class UpdatePackageDownloader final : public base::has_weak_ptr {
public:
	void start(const ManifestInfo &manifest);
	void stop();
	void restoreFromDisk(qint64 announcedVersionCode);

	[[nodiscard]] std::optional<ReadyPackage> ready() const {
		return _ready;
	}
	[[nodiscard]] bool downloading() const {
		return _reply != nullptr;
	}
	[[nodiscard]] Core::UpdateChecker::Progress progress() const {
		return { _already, _total, false };
	}

	[[nodiscard]] rpl::producer<Core::UpdateChecker::Progress> progressed() const {
		return _progress.events();
	}
	[[nodiscard]] rpl::producer<ReadyPackage> readied() const {
		return _readied.events();
	}
	[[nodiscard]] rpl::producer<> failed() const {
		return _failed.events();
	}

private:
	void abortRequest();
	void finish(not_null<QNetworkReply*> reply);
	void fail();
	void publish(const ReadyPackage &package);
	void writeSidecar(const ReadyPackage &package, const QString &sha256);

	std::unique_ptr<QNetworkAccessManager> _manager;
	QPointer<QNetworkReply> _reply;
	std::unique_ptr<QSaveFile> _file;
	std::unique_ptr<QCryptographicHash> _hash;

	ManifestInfo _manifest;
	QString _targetPath;
	qint64 _already = 0;
	qint64 _total = 0;
	bool _oversized = false;

	std::optional<ReadyPackage> _ready;

	rpl::event_stream<Core::UpdateChecker::Progress> _progress;
	rpl::event_stream<ReadyPackage> _readied;
	rpl::event_stream<> _failed;

};

void UpdatePackageDownloader::start(const ManifestInfo &manifest) {
	if (!manifest.asset.valid()) {
		return;
	} else if (_ready && _ready->versionCode == manifest.versionCode) {
		return;
	} else if (_reply && _manifest.versionCode == manifest.versionCode) {
		return;
	}
	abortRequest();

	const auto url = QUrl(manifest.asset.url);
	if (!url.isValid() || !IsAllowedManifestUrl(url)) {
		LOG(("Update Error: FoxMes package url is not allowed."));
		fail();
		return;
	}
	const auto folder = UpdatesFolder();
	if (!QDir().mkpath(folder)) {
		LOG(("Update Error: FoxMes cannot create the updates folder."));
		fail();
		return;
	}
	auto name = QFileInfo(url.path()).fileName();
	if (name.isEmpty()) {
		name = u"package"_q;
	}
	_manifest = manifest;
	_targetPath = folder + name;
	_already = 0;
	_total = 0;
	_oversized = false;

	_file = std::make_unique<QSaveFile>(_targetPath);
	if (!_file->open(QIODevice::WriteOnly)) {
		LOG(("Update Error: FoxMes cannot write '%1'.").arg(_targetPath));
		_file = nullptr;
		fail();
		return;
	}
	_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);

	if (!_manager) {
		_manager = std::make_unique<QNetworkAccessManager>();
	}
	auto request = QNetworkRequest(url);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setMaximumRedirectsAllowed(5);
	request.setTransferTimeout(kPackageTimeoutMs);

	DEBUG_LOG(("Update Info: FoxMes downloading %1.").arg(manifest.version));
	const auto reply = _manager->get(request);
	_reply = reply;
	const auto weak = base::make_weak(this);
	QObject::connect(reply, &QNetworkReply::downloadProgress, [=](
			qint64 received,
			qint64 total) {
		if (!weak || _reply != reply) {
			return;
		}
		_total = total;
		_progress.fire({ _already, _total, false });
	});
	QObject::connect(reply, &QNetworkReply::readyRead, [=] {
		if (!weak || _reply != reply || !_file) {
			return;
		}
		const auto chunk = reply->readAll();
		if (chunk.isEmpty()) {
			return;
		} else if (_already + chunk.size() > kMaxPackageSize) {
			// Stop writing right here rather than after the fact: an answer
			// that keeps growing must not be allowed to fill the disk.
			LOG(("Update Error: FoxMes package is too large."));
			_oversized = true;
			reply->abort();
			return;
		}
		_file->write(chunk);
		_hash->addData(chunk);
		_already += chunk.size();
		_progress.fire({ _already, _total, false });
	});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		if (weak) {
			finish(reply);
		}
	});
	_progress.fire({ 0, 0, false });
}

void UpdatePackageDownloader::abortRequest() {
	if (const auto reply = base::take(_reply)) {
		reply->disconnect();
		reply->abort();
		reply->deleteLater();
	}
	if (const auto file = base::take(_file)) {
		file->cancelWriting();
	}
	_hash = nullptr;
}

void UpdatePackageDownloader::stop() {
	abortRequest();
	_ready = std::nullopt;
	_already = 0;
	_total = 0;
}

void UpdatePackageDownloader::finish(not_null<QNetworkReply*> reply) {
	if (_reply != reply) {
		return;
	}
	_reply = nullptr;
	const auto guard = gsl::finally([&] { reply->deleteLater(); });
	const auto file = base::take(_file);
	const auto hash = base::take(_hash);

	if (_oversized || reply->error() != QNetworkReply::NoError) {
		if (!_oversized) {
			LOG(("Update Error: FoxMes package request failed: %1"
				).arg(int(reply->error())));
		}
		if (file) {
			file->cancelWriting();
		}
		fail();
		return;
	} else if (!IsAllowedManifestUrl(reply->url())) {
		LOG(("Update Error: FoxMes package unsafe final url."));
		if (file) {
			file->cancelWriting();
		}
		fail();
		return;
	} else if (!file || !hash) {
		fail();
		return;
	}

	const auto digest = QString::fromLatin1(hash->result().toHex());
	if (digest.compare(_manifest.asset.sha256, Qt::CaseInsensitive) != 0) {
		LOG(("Update Error: FoxMes package sha256 mismatch, expected %1 got %2"
			).arg(_manifest.asset.sha256).arg(digest));
		file->cancelWriting();
		fail();
		return;
	}
	if (!file->commit()) {
		LOG(("Update Error: FoxMes cannot commit '%1'.").arg(_targetPath));
		fail();
		return;
	}

	auto package = ReadyPackage{
		_manifest.versionCode,
		_manifest.version,
		_targetPath,
	};
	writeSidecar(package, digest);
	publish(package);
}

void UpdatePackageDownloader::publish(const ReadyPackage &package) {
	DEBUG_LOG(("Update Info: FoxMes package %1 verified.").arg(package.version));
	_ready = package;
	_readied.fire_copy(package);
}

void UpdatePackageDownloader::writeSidecar(
		const ReadyPackage &package,
		const QString &sha256) {
	auto file = QSaveFile(SidecarPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	const auto document = QJsonDocument(QJsonObject{
		{ u"version_code"_q, double(package.versionCode) },
		{ u"version"_q, package.version },
		{ u"sha256"_q, sha256 },
		{ u"path"_q, package.path },
	});
	file.write(document.toJson(QJsonDocument::Compact));
	file.commit();
}

// A package verified by an earlier run is trusted only after its digest is
// checked again: the previous process may have died mid-write, and re-hashing
// a local file costs nothing compared with downloading it twice.
void UpdatePackageDownloader::restoreFromDisk(qint64 announcedVersionCode) {
	if (_ready || _reply) {
		return;
	}
	auto file = QFile(SidecarPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	file.close();
	if (!document.isObject()) {
		ClearFolder();
		return;
	}
	const auto object = document.object();
	const auto versionCode = qint64(
		object.value(u"version_code"_q).toDouble());
	const auto version = object.value(u"version"_q).toString();
	const auto sha256 = object.value(u"sha256"_q).toString();
	const auto path = object.value(u"path"_q).toString();
	if (versionCode <= AppVersion
		|| (announcedVersionCode && versionCode != announcedVersionCode)
		|| version.isEmpty()
		|| sha256.isEmpty()
		|| path.isEmpty()
		|| !QFile::exists(path)) {
		ClearFolder();
		return;
	}
	if (Sha256OfFile(path).compare(sha256, Qt::CaseInsensitive) != 0) {
		LOG(("Update Error: FoxMes stored package failed verification."));
		ClearFolder();
		return;
	}
	publish(ReadyPackage{ versionCode, version, path });
}

void UpdatePackageDownloader::fail() {
	abortRequest();
	// ClearFolder takes the previously verified package with it, so the ready
	// state has to go too: leaving it set would offer an install of a file
	// that is no longer on disk.
	ClearFolder();
	_ready = std::nullopt;
	_failed.fire({});
}

} // namespace

namespace {

[[nodiscard]] std::optional<ManifestInfo> ParseManifest(
		const QByteArray &json) {
	auto error = QJsonParseError{ 0, QJsonParseError::NoError };
	const auto document = QJsonDocument::fromJson(json, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		LOG(("Update Error: FoxMes GitHub bad manifest JSON."));
		return std::nullopt;
	}
	const auto object = document.object();
	const auto codeValue = object.constFind("version_code");
	if (codeValue == object.constEnd() || !codeValue->isDouble()) {
		LOG(("Update Error: FoxMes GitHub missing version_code."));
		return std::nullopt;
	}
	const auto rawCode = codeValue->toDouble();
	if (!(rawCode >= 0.0)
		|| std::floor(rawCode) != rawCode
		|| rawCode > double(std::numeric_limits<int>::max())) {
		LOG(("Update Error: FoxMes GitHub invalid version_code."));
		return std::nullopt;
	}
	const auto versionValue = object.constFind("version");
	if (versionValue == object.constEnd() || !versionValue->isString()) {
		LOG(("Update Error: FoxMes GitHub missing version string."));
		return std::nullopt;
	}
	const auto version = versionValue->toString();
	if (version.isEmpty() || version.size() > 64) {
		LOG(("Update Error: FoxMes GitHub invalid version string."));
		return std::nullopt;
	}

	auto asset = PackageAsset();
	const auto key = PlatformKey();
	if (!key.isEmpty()) {
		const auto platformValue = object.constFind(key);
		if (platformValue != object.constEnd() && platformValue->isObject()) {
			const auto platform = platformValue->toObject();
			const auto url = platform.value(u"url"_q).toString();
			const auto sha256 = platform.value(u"sha256"_q).toString();
			// 64 hex characters, nothing else: a digest we cannot parse must
			// not silently degrade into "no verification".
			const auto digestOk = (sha256.size() == 64)
				&& std::all_of(sha256.begin(), sha256.end(), [](QChar ch) {
					return ch.isDigit()
						|| (ch >= 'a' && ch <= 'f')
						|| (ch >= 'A' && ch <= 'F');
				});
			if (!url.isEmpty() && digestOk && IsAllowedManifestUrl(QUrl(url))) {
				asset = PackageAsset{ url, sha256 };
			} else {
				LOG(("Update Error: FoxMes manifest has a bad %1 block."
					).arg(key));
				return std::nullopt;
			}
		} else if (platformValue != object.constEnd()) {
			return std::nullopt;
		}
	}

	return ManifestInfo{ qint64(rawCode), version, asset };
}

[[nodiscard]] std::optional<AvailableUpdate> ParseDesktopVersion(
		const QJsonDocument &document) {
	if (!document.isObject()) {
		return std::nullopt;
	}
	const auto object = document.object();
	const auto codeValue = object.constFind("version_code");
	if (codeValue == object.constEnd() || !codeValue->isDouble()) {
		return std::nullopt;
	}
	const auto rawCode = codeValue->toDouble();
	if (!(rawCode >= 0.0)
		|| std::floor(rawCode) != rawCode
		|| rawCode > double(std::numeric_limits<int>::max())) {
		return std::nullopt;
	}
	const auto versionValue = object.constFind("version");
	if (versionValue == object.constEnd() || !versionValue->isString()) {
		return std::nullopt;
	}
	const auto version = versionValue->toString();
	if (version.isEmpty() || version.size() > 64) {
		return std::nullopt;
	}
	return AvailableUpdate{ qint64(rawCode), version };
}

class GitHubUpdateChecker final : public base::has_weak_ptr {
public:
	GitHubUpdateChecker() : _timer([=] { send(); }) {
	}

	void start() {
		if (_checkingRequest) {
			return;
		}
		_timer.cancel();
		if (!_manager) {
			_manager = std::make_unique<QNetworkAccessManager>();
		}
		// A package left by an earlier run is restored only once fxl-api says
		// which version is allowed: restoring it here would offer a build the
		// administrator may have since rolled back.
		watchDownloader();
		send();
	}

	// Only an explicit "turn updates off" request stops the poll. The
	// upstream Updater is owned by short-lived Core::UpdateChecker values
	// (MainWidget, Intro::Widget and the settings section all create one on
	// the stack), so tying this to its destructor aborted the very request
	// that had just been started, and nothing ever rescheduled it.
	void stop() {
		++_generation;
		_checkingRequest = false;
		_downloadRequested = false;
		_phase = Phase::Idle;
		_manifest.reset();
		_timer.cancel();
		abortRequest();
		_downloader.stop();
		_last.reset();
		_changed.fire({});
	}

	void download();
	void installFailed();
	[[nodiscard]] Status status() const;
	[[nodiscard]] rpl::producer<> changed() const {
		return rpl::merge(
			_changed.events(),
			_downloader.progressed() | rpl::to_empty);
	}

	[[nodiscard]] std::optional<AvailableUpdate> last() const {
		return _last;
	}
	[[nodiscard]] UpdatePackageDownloader &downloader() {
		return _downloader;
	}

	[[nodiscard]] rpl::producer<> checking() const {
		return _checking.events();
	}
	[[nodiscard]] rpl::producer<> isLatest() const {
		return _isLatest.events();
	}
	[[nodiscard]] rpl::producer<> failed() const {
		return rpl::merge(_failed.events(), _downloader.failed());
	}
	[[nodiscard]] rpl::producer<AvailableUpdate> available() const {
		return _available.events();
	}

private:
	void send();
	void requestManifest();
	void handleManifest(not_null<QNetworkReply*> reply);
	void watchDownloader();
	void reschedule();
	void fail();

	std::unique_ptr<QNetworkAccessManager> _manager;
	QPointer<QNetworkReply> _reply;
	base::Timer _timer;
	std::optional<AvailableUpdate> _last;
	// What fxl-api last allowed. The GitHub manifest must agree with it.
	qint64 _announcedVersionCode = 0;
	UpdatePackageDownloader _downloader;
	bool _watching = false;
	bool _checkingRequest = false;
	bool _downloadRequested = false;
	int _generation = 0;
	Phase _phase = Phase::Idle;
	std::optional<ManifestInfo> _manifest;
	rpl::event_stream<> _changed;
	rpl::lifetime _lifetime;

	rpl::event_stream<> _checking;
	rpl::event_stream<> _isLatest;
	rpl::event_stream<> _failed;
	rpl::event_stream<AvailableUpdate> _available;

	void abortRequest() {
		if (const auto reply = base::take(_reply)) {
			reply->disconnect();
			reply->abort();
			reply->deleteLater();
		}
	}

};

void GitHubUpdateChecker::watchDownloader() {
	if (_watching) {
		return;
	}
	_watching = true;
	// A verified package re-announces availability: the settings section
	// subscribes to this and swaps the "downloading" label back for the
	// update button, which now installs instead of opening the browser.
	_downloader.readied(
	) | rpl::on_next([=](ReadyPackage package) {
		auto update = AvailableUpdate{ package.versionCode, package.version };
		_last = update;
		_phase = Phase::Ready;
		_available.fire(std::move(update));
		_changed.fire({});
	}, _lifetime);
	_downloader.failed() | rpl::on_next([=] {
		_phase = Phase::Failed;
		_changed.fire({});
	}, _lifetime);
}

void GitHubUpdateChecker::reschedule() {
	// Counted from the moment the previous check completes, regardless of its
	// outcome: a slow or failed check must not make the next one overlap it.
	_timer.callOnce(kRecheckInterval);
}

Status GitHubUpdateChecker::status() const {
	const auto phase = _downloader.downloading()
		? Phase::Downloading
		: (_phase == Phase::Failed)
		? Phase::Failed
		: _downloader.ready()
		? Phase::Ready
		: _phase;
	return {
		phase,
		_last.value_or(AvailableUpdate()),
		_downloader.progress(),
		PlatformKey().isEmpty() || (_manifest && !_manifest->asset.valid()),
	};
}

void GitHubUpdateChecker::download() {
	if (_downloader.downloading() || _downloader.ready()) {
		return;
	}
	_downloadRequested = true;
	start();
}

void GitHubUpdateChecker::installFailed() {
	if (const auto ready = _downloader.ready()
		; ready && !QFile::exists(ready->path)) {
		_downloader.stop();
	}
	_phase = Phase::Failed;
	_changed.fire({});
}

void GitHubUpdateChecker::fail() {
	_checkingRequest = false;
	_downloadRequested = false;
	_phase = Phase::Failed;
	_changed.fire({});
	// A failed check says nothing about the release we already saw:
	// keep the known available update so a network blip does not hide
	// the update button until the next successful check.
	_failed.fire({});
}

void GitHubUpdateChecker::send() {
	_checkingRequest = true;
	_phase = Phase::Checking;
	_changed.fire({});
	DEBUG_LOG(("Update Info: FoxMes asking the API for the desktop version."));
	_checking.fire({});

	const auto weak = base::make_weak(this);
	const auto generation = _generation;
	Client().desktopVersion([=](QJsonDocument doc, QString error, int status) {
		if (!weak || generation != _generation) {
			return;
		}
		if (!error.isEmpty()) {
			LOG(("Update Error: FoxMes version request failed: %1 (%2)"
				).arg(error).arg(status));
			fail();
			reschedule();
			return;
		}
		const auto announced = ParseDesktopVersion(doc);
		if (!announced) {
			LOG(("Update Error: FoxMes bad version response."));
			fail();
			reschedule();
			return;
		}
		if (_announcedVersionCode != announced->versionCode) {
			_downloader.stop();
			_manifest.reset();
			_last.reset();
		}
		_announcedVersionCode = announced->versionCode;
		if (announced->versionCode <= AppVersion) {
			DEBUG_LOG(("Update Info: FoxMes %1 (%2) is up to date."
				).arg(announced->versionCode).arg(announced->version));
			_last.reset();
			_downloader.stop();
			_checkingRequest = false;
			_downloadRequested = false;
			_phase = Phase::Latest;
			_changed.fire({});
			_isLatest.fire({});
			reschedule();
			return;
		}
		// The administrator allowed this version: a package verified earlier
		// for exactly it can be offered without touching the network at all.
		_downloader.restoreFromDisk(announced->versionCode);
		if (const auto ready = _downloader.ready()) {
			if (ready->versionCode == announced->versionCode) {
				auto update = AvailableUpdate{
					ready->versionCode,
					ready->version,
				};
				_last = update;
				_checkingRequest = false;
				_downloadRequested = false;
				_phase = Phase::Ready;
				_changed.fire({});
				_available.fire(std::move(update));
				reschedule();
				return;
			}
		}
		requestManifest();
	});
}

void GitHubUpdateChecker::requestManifest() {
	abortRequest();
	if (!_manager) {
		_manager = std::make_unique<QNetworkAccessManager>();
	}
	auto request = QNetworkRequest(QUrl(ManifestUrl()));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setMaximumRedirectsAllowed(5);
	request.setTransferTimeout(kRequestTimeoutMs);

	DEBUG_LOG(("Update Info: FoxMes checking GitHub manifest."));

	const auto reply = _manager->get(request);
	_reply = reply;
	const auto weak = base::make_weak(this);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		if (weak) {
			handleManifest(reply);
		}
	});
}

void GitHubUpdateChecker::handleManifest(not_null<QNetworkReply*> reply) {
	if (_reply != reply) {
		return;
	}
	_reply = nullptr;
	const auto guard = gsl::finally([&] {
		reply->deleteLater();
		reschedule();
	});

	if (reply->error() != QNetworkReply::NoError) {
		LOG(("Update Error: FoxMes GitHub request failed: %1"
			).arg(int(reply->error())));
		fail();
		return;
	} else if (!IsAllowedManifestUrl(reply->url())) {
		LOG(("Update Error: FoxMes GitHub unsafe final url."));
		fail();
		return;
	}
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (status < 200 || status >= 300) {
		LOG(("Update Error: FoxMes GitHub bad HTTP status: %1").arg(status));
		fail();
		return;
	}

	const auto data = reply->readAll();
	if (data.size() > kMaxManifestSize) {
		LOG(("Update Error: FoxMes GitHub manifest too large: %1"
			).arg(data.size()));
		fail();
		return;
	}
	const auto manifest = ParseManifest(data);
	if (!manifest) {
		fail();
		return;
	} else if (manifest->versionCode != _announcedVersionCode) {
		// The rollout was allowed for another build. Installing whatever the
		// release page happens to hold right now is not what was approved.
		LOG(("Update Error: FoxMes manifest %1 differs from allowed %2."
			).arg(manifest->versionCode).arg(_announcedVersionCode));
		fail();
		return;
	}

	DEBUG_LOG(("Update Info: FoxMes update available %1 (%2)."
		).arg(manifest->versionCode).arg(manifest->version));
	const auto notify = !_last
		|| (_last->versionCode != manifest->versionCode);
	auto update = AvailableUpdate{ manifest->versionCode, manifest->version };
	_last = update;
	if (notify) {
		Ui::Toast::Show(u"FoxMes Desktop %1 is available."_q.arg(
			update.version));
	}
	_manifest = *manifest;
	_checkingRequest = false;
	_phase = Phase::Available;
	const auto download = cAutoUpdate() || _downloadRequested;
	_downloadRequested = false;
	_available.fire(std::move(update));
	if (download) {
		_downloader.start(*manifest);
	}
	_changed.fire({});
}

std::shared_ptr<GitHubUpdateChecker> InstanceValue;

[[nodiscard]] std::shared_ptr<GitHubUpdateChecker> Instance() {
	// The checker owns a QNetworkAccessManager, which must not be destroyed
	// after QCoreApplication: its worker thread would then be joined with
	// nothing left to tell it to quit, and exit() would never return.
	[[maybe_unused]] static const auto release = [] {
		ReleaseOnQuit([] { InstanceValue = nullptr; });
		return true;
	}();
	if (!InstanceValue) {
		InstanceValue = std::make_shared<GitHubUpdateChecker>();
	}
	return InstanceValue;
}

[[nodiscard]] QString InstallLaunch(const QString &package) {
#ifdef Q_OS_WIN
	// Per-user install ({localappdata}, PrivilegesRequired=lowest), so the
	// silent installer needs no elevation. It closes this process and starts
	// the new build itself - see foxmes.iss.
	const auto native = QDir::toNativeSeparators(package);
	const auto result = ShellExecuteW(
		nullptr,
		L"open",
		reinterpret_cast<const wchar_t*>(native.utf16()),
		L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART",
		nullptr,
		SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(result) <= 32) {
		return u"ShellExecute failed"_q;
	}
	return QString();
#elif defined Q_OS_MAC // Q_OS_WIN
	// The bundle cannot rewrite itself while it is running, so a detached
	// helper waits for this process to exit, swaps the bundle, strips the
	// quarantine attribute and relaunches.
	// BundledResourcesPath() already points inside the bundle, at
	// FoxMes.app/Contents/Resources - the same place base::RegisterBundledResources
	// loads the .rcc files from. Appending that suffix again is what broke the
	// macOS install path in 1.5.0 and 1.6.0.
	const auto script = base::Platform::BundledResourcesPath()
		+ u"/fox_update_macos.sh"_q;
	if (!QFile::exists(script)) {
		return u"helper script is missing"_q;
	}
	const auto bundle = cExeDir() + cExeName();
	if (!QProcess::startDetached(u"/bin/bash"_q, QStringList{
		script,
		package,
		bundle,
		QString::number(QCoreApplication::applicationPid()),
	})) {
		return u"cannot start the helper"_q;
	}
	return QString();
#else // Q_OS_MAC
	// Replacing a running executable is allowed here: rename() only relinks
	// the directory entry, and this process keeps running from the old inode
	// until it exits on its own.
	const auto current = qEnvironmentVariable("APPIMAGE");
	if (current.isEmpty()) {
		return u"not running as an AppImage"_q;
	}
	auto file = QFile(package);
	if (!file.setPermissions(file.permissions()
		| QFileDevice::ExeOwner
		| QFileDevice::ExeGroup
		| QFileDevice::ExeOther)) {
		return u"cannot make the package executable"_q;
	}
	if (!base::Platform::RenameWithOverwrite(package, current)) {
		return u"cannot replace the AppImage"_q;
	}
	if (!QProcess::startDetached(current, QStringList())) {
		return u"cannot start the new AppImage"_q;
	}
	return QString();
#endif // else for Q_OS_WIN || Q_OS_MAC
}

} // namespace

Status CurrentStatus() {
	return Instance()->status();
}

rpl::producer<Status> StatusValue() {
	return rpl::single(rpl::empty) | rpl::then(
		Instance()->changed()
	) | rpl::map([] { return CurrentStatus(); });
}

bool SupportsSelfUpdate() {
	return !PlatformKey().isEmpty();
}

void SetAutomaticDownload(bool enabled) {
	if (cAutoUpdate() == enabled) {
		return;
	}
	cSetAutoUpdate(enabled);
	Local::writeSettings();
	if (enabled) {
		Instance()->start();
	}
}

void DownloadUpdate() {
	Instance()->download();
}

void StartUpdateCheck() {
	Instance()->start();
}

void StopUpdateCheck() {
	if (InstanceValue) {
		InstanceValue->stop();
	}
}

bool IsAvailable() {
	return InstanceValue && InstanceValue->last().has_value();
}

AvailableUpdate CurrentAvailable() {
	if (InstanceValue) {
		return InstanceValue->last().value_or(AvailableUpdate());
	}
	return {};
}

void OpenReleasePage() {
	File::OpenUrl(ReleasePageUrl());
}

bool IsReadyToInstall() {
	return InstanceValue
		&& InstanceValue->downloader().ready().has_value();
}

void InstallAndRestart() {
	if (!InstanceValue) {
		return;
	}
	const auto ready = InstanceValue->downloader().ready();
	if (!ready) {
		return;
	}
	const auto giveUp = [] {
		InstanceValue->installFailed();
		Ui::Toast::Show(tr::lng_settings_update_fail(tr::now));
	};
	if (!QFile::exists(ready->path)) {
		// Checked here rather than left to the platform: the macOS helper is
		// detached, so a missing package there would quit the app and fail
		// out of sight.
		LOG(("Update Error: FoxMes package '%1' is gone.").arg(ready->path));
		giveUp();
		return;
	}
	const auto error = InstallLaunch(ready->path);
	if (!error.isEmpty()) {
		LOG(("Update Error: FoxMes cannot install: %1").arg(error));
		giveUp();
		return;
	}
	Core::Quit();
}

rpl::producer<> CheckingEvents() {
	return Instance()->checking();
}

rpl::producer<> IsLatestEvents() {
	return Instance()->isLatest();
}

rpl::producer<> FailedEvents() {
	return Instance()->failed();
}

rpl::producer<AvailableUpdate> AvailableEvents() {
	return Instance()->available();
}

auto ProgressEvents() -> rpl::producer<Core::UpdateChecker::Progress> {
	return Instance()->downloader().progressed();
}

Core::UpdateChecker::State CurrentState() {
	if (InstanceValue && InstanceValue->downloader().downloading()) {
		return Core::UpdateChecker::State::Download;
	}
	return Core::UpdateChecker::State::None;
}

int AlreadyDownloaded() {
	return InstanceValue
		? int(InstanceValue->downloader().progress().already)
		: 0;
}

int TotalSize() {
	return InstanceValue
		? int(InstanceValue->downloader().progress().size)
		: 0;
}

bool PreferPercent() {
	return false;
}

} // namespace CustomBackend::Updates
