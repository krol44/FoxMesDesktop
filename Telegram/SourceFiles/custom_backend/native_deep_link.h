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

} // namespace CustomBackend::DeepLinks
