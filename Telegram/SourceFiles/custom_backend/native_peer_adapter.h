/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QString>
#include <functional>

class UserData;

namespace CustomBackend::Peers {

bool UpdateProfileName(
	not_null<UserData*> user,
	const QString &first,
	const QString &last,
	std::function<void(QString error)> done);

}
