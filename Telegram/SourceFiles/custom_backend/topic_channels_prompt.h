/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
}

namespace CustomBackend::TopicChannels {

void OfferChannels(
	not_null<Window::SessionController*> window,
	Fn<void()> shown);

}
