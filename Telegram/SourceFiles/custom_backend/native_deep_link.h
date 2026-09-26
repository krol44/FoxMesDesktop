/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class QUrl;
class QString;

namespace CustomBackend::DeepLinks {

void RegisterScheme();
[[nodiscard]] bool HandleStartUrl(const QUrl &url);
[[nodiscard]] bool StartUrlRequiresActivate(const QString &url);

// FoxMes's internal links live under foxtail.ing/@ (MTP::ConfigFields), which
// upstream's t.me patterns in TryConvertUrlToLocal do not know. Returns the
// t.me form of such a link, so the client opens it itself - a user, a
// public chat (with ?videochat / ?livestream a call) or, as c/<id>, a
// private one - and any other url unchanged. fxl.ru/@ is accepted as well:
// older messages still carry the previous domain.
[[nodiscard]] QString LocalizeInternalLink(const QString &url);

} // namespace CustomBackend::DeepLinks
