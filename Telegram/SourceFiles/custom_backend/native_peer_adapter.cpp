/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/native_peer_adapter.h"

#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "data/data_peer.h"
#include "data/data_user.h"

#include <QJsonObject>

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
		std::function<void(QString error)> done) {
	const auto bridge = BridgeFor(&user->session());
	if (!bridge) {
		return false;
	}
	const auto display = (first + u" "_q + last).trimmed();
	bridge->updateProfile(
		display,
		[done = std::move(done)](QJsonObject, QString error) {
			done(std::move(error));
		});
	return true;
}

} // namespace CustomBackend::Peers
