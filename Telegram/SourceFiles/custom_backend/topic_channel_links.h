/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_msg_id.h"

class HistoryItem;

namespace qthelp {
class RegularExpressionMatch;
}

namespace Main {
class Session;
}

namespace Window {
class SessionController;
}

namespace CustomBackend::TopicChannels {


void RememberSiteLink(
	not_null<Main::Session*> session,
	FullMsgId itemId,
	const QString &url);
[[nodiscard]] QString SiteLink(not_null<HistoryItem*> item);

[[nodiscard]] QString LocalSiteLink(const QString &url);
bool OpenSiteLink(
	Window::SessionController *controller,
	const qthelp::RegularExpressionMatch &match,
	const QVariant &context);

}
