/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/github_update.h"

#include "base/algorithm.h"
#include "base/debug_log.h"
#include "base/timer.h"
#include "core/file_utilities.h"
#include "core/version.h"
#include "custom_backend/native_runtime.h"
#include "ui/toast/toast.h"

#include <gsl/gsl>

#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <cmath>
#include <limits>
#include <optional>

namespace CustomBackend::Updates {
namespace {

const auto kManifestUrl = u"https://github.com/krol44/FoxMesDesktop/"_q
	+ u"releases/latest/download/version.json"_q;
const auto kReleasePageUrl = u"https://github.com/krol44/FoxMesDesktop/"_q
	+ u"releases/latest"_q;

// Next check exactly one hour after the previous one completes.
constexpr auto kRequestTimeoutMs = 30 * 1000;
constexpr auto kMaxManifestSize = 16 * 1024;
constexpr auto kRecheckInterval = 60 * 60 * crl::time(1000);

QString ManifestUrl() {
	return kManifestUrl;
}

QString ReleasePageUrl() {
	return kReleasePageUrl;
}

[[nodiscard]] bool IsAllowedManifestUrl(const QUrl &url) {
	const auto host = url.host().toLower();
	return (url.scheme() == u"https"_q)
		&& (url.port(-1) == -1 || url.port() == 443)
		&& url.userInfo().isEmpty()
		&& ((host == u"github.com"_q)
			|| host.endsWith(u".githubusercontent.com"_q));
}

class GitHubUpdateChecker final : public base::has_weak_ptr {
public:
	GitHubUpdateChecker() : _timer([=] { send(); }) {
	}

	void start() {
		_timer.cancel();
		abortRequest();
		if (!_manager) {
			_manager = std::make_unique<QNetworkAccessManager>();
		}
		send();
	}

	void stop() {
		_timer.cancel();
		abortRequest();
		_last.reset();
	}

	[[nodiscard]] std::optional<AvailableUpdate> last() const {
		return _last;
	}

	[[nodiscard]] rpl::producer<> checking() const {
		return _checking.events();
	}

	[[nodiscard]] rpl::producer<> isLatest() const {
		return _isLatest.events();
	}

	[[nodiscard]] rpl::producer<> failed() const {
		return _failed.events();
	}

	[[nodiscard]] rpl::producer<AvailableUpdate> available() const {
		return _available.events();
	}

private:
	void send() {
		auto request = QNetworkRequest(QUrl(ManifestUrl()));
		request.setAttribute(
			QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::NoLessSafeRedirectPolicy);
		request.setMaximumRedirectsAllowed(5);
		request.setTransferTimeout(kRequestTimeoutMs);

		DEBUG_LOG(("Update Info: FoxMes checking GitHub manifest."));
		_checking.fire({});

		const auto reply = _manager->get(request);
		_reply = reply;
		QObject::connect(reply, &QNetworkReply::finished, [=] {
			handleFinished(reply);
		});
	}

	void abortRequest() {
		if (const auto reply = base::take(_reply)) {
			reply->disconnect(reply, &QNetworkReply::finished, nullptr, nullptr);
			reply->abort();
			reply->deleteLater();
		}
	}

	void handleFinished(not_null<QNetworkReply*> reply) {
		if (_reply != reply) {
			return;
		}
		_reply = nullptr;
		const auto guard = gsl::finally([&] {
			reply->deleteLater();
			// Next check exactly one hour after the previous one completes,
			// regardless of the outcome.
			_timer.callOnce(kRecheckInterval);
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
		const auto update = ParseManifest(data);
		if (!update) {
			fail();
			return;
		} else if (update->versionCode <= AppVersion) {
			DEBUG_LOG(("Update Info: FoxMes version %1 (%2) is up to date."
				).arg(update->versionCode).arg(update->version));
			_last.reset();
			_isLatest.fire({});
			return;
		}
		DEBUG_LOG(("Update Info: FoxMes update available %1 (%2)."
			).arg(update->versionCode).arg(update->version));
		const auto notify = !_last
			|| (_last->versionCode != update->versionCode);
		_last = update;
		if (notify) {
			Ui::Toast::Show(u"FoxMes Desktop %1 is available."_q.arg(
				update->version));
		}
		auto result = *update;
		_available.fire(std::move(result));
	}

	void fail() {
		_last.reset();
		_failed.fire({});
	}

	static std::optional<AvailableUpdate> ParseManifest(
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
		// The URL to open is always the baked-in GitHub page, never taken
		// from the remote JSON.
		return AvailableUpdate{
			qint64(rawCode),
			version,
		};
	}

	std::unique_ptr<QNetworkAccessManager> _manager;
	QPointer<QNetworkReply> _reply;
	base::Timer _timer;
	std::optional<AvailableUpdate> _last;

	rpl::event_stream<> _checking;
	rpl::event_stream<> _isLatest;
	rpl::event_stream<> _failed;
	rpl::event_stream<AvailableUpdate> _available;

};

std::shared_ptr<GitHubUpdateChecker> InstanceValue;

std::shared_ptr<GitHubUpdateChecker> Instance() {
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

} // namespace

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

} // namespace CustomBackend::Updates
