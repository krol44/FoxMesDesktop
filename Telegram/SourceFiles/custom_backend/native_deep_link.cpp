/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/native_deep_link.h"

#include "base/platform/base_platform_url_scheme.h"
#include "core/application.h"
#include "core/launcher.h"
#include "core/version.h"
#include "platform/platform_specific.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>

namespace CustomBackend::DeepLinks {
namespace {

[[nodiscard]] bool IsOpenRoute(const QUrl &url) {
	return (url.scheme() == u"foxmes"_q)
		&& (url.host() == u"open"_q)
		&& (url.path().isEmpty() || url.path() == u"/"_q)
		&& !url.hasQuery()
		&& !url.hasFragment();
}

} // namespace

void RegisterScheme() {
	const auto arguments = Core::Launcher::Instance().customWorkingDir()
		? u"-workdir \"%1\""_q.arg(cWorkingDir())
		: QString();

	base::Platform::RegisterUrlScheme(base::Platform::UrlSchemeDescriptor{
		.executable = Platform::ExecutablePathForShortcuts(),
		.arguments = arguments,
		.protocol = u"foxmes"_q,
		.protocolName = u"FoxMes Link"_q,
		.shortAppName = u"foxmes"_q,
		.longAppName = QCoreApplication::applicationName(),
		.displayAppName = AppName.utf16(),
		.displayAppDescription = u"FoxMes Desktop"_q,
	});
}

bool HandleStartUrl(const QUrl &url) {
	if (url.isLocalFile() || url.scheme() == u"interpret"_q) {
		return false;
	}
	if (url.scheme() == u"foxmes"_q && !IsOpenRoute(url)) {
		if (!Core::App().activePrimaryWindow()) {
			return false;
		}
		auto local = url;
		local.setScheme(u"tg"_q);
		Core::App().openLocalUrl(local.toString(), {});
	}
	return true;
}

QString LocalizeInternalLink(const QString &url) {
	static const auto Internal = QRegularExpression(
		u"^(https?://)?(www\\.)?(foxtail\\.ing|fxl\\.ru)/(@|(?=(addemoji|addstickers)/))(.+)$"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto match = Internal.match(url);
	return match.hasMatch()
		? (match.captured(1) + u"t.me/"_q + match.captured(6))
		: url;
}

QString InternalLinksDomain(const QString &domain, const QString &query) {
	static const auto SetLink = QRegularExpression(
		u"^(addemoji|addstickers)/"_q,
		QRegularExpression::CaseInsensitiveOption);
	return (domain.endsWith(u"/@"_q) && SetLink.match(query).hasMatch())
		? domain.chopped(1)
		: domain;
}

bool StartUrlRequiresActivate(const QString &url) {
	const auto parsed = QUrl(url);
	return (parsed.scheme() == u"foxmes"_q)
		|| parsed.isLocalFile()
		|| parsed.scheme() == u"interpret"_q;
}

}
