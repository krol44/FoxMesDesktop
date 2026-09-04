/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/native_deep_link.h"

#include "base/platform/base_platform_url_scheme.h"
#include "core/launcher.h"
#include "core/version.h"
#include "platform/platform_specific.h"

#include <QtCore/QCoreApplication>
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

	return true;
}

bool StartUrlRequiresActivate(const QString &url) {
	const auto parsed = QUrl(url);
	return IsOpenRoute(parsed)
		|| parsed.isLocalFile()
		|| parsed.scheme() == u"interpret"_q;
}

} // namespace CustomBackend::DeepLinks
