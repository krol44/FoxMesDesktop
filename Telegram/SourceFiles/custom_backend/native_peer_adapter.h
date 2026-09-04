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

// Returns false when the bridge is unavailable; in that case the caller must
// leave its state untouched (upstream silently does nothing). Transport
// details (display name composition) live here, UI reaction callbacks stay in
// the calling boxes. Group/channel creation is intentionally absent: FoxMes
// supports only direct dialogs and Saved Messages.
bool UpdateProfileName(
	not_null<UserData*> user,
	const QString &first,
	const QString &last,
	std::function<void(QString error)> done);

} // namespace CustomBackend::Peers
