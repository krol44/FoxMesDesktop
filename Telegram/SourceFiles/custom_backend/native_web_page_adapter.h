#pragma once

#include <QString>

#include <functional>

namespace Main {
class Session;
}

namespace CustomBackend {

// Serves the composer's link preview under the bridge. Upstream resolves it
// with messages.getWebPagePreview, which never completes here; fxl-api reads
// the page instead, so the client still never touches a third-party host and
// the card comes back in the very shape upstream expects.
void RequestWebPagePreview(
	Main::Session *session,
	const QString &link,
	std::function<void(const MTPDmessageMediaWebPage&)> done,
	std::function<void()> fail);

// Drops an in-flight preview for a link the composer no longer shows. The REST
// client has no per-request handle to cancel, so the answer is discarded on
// arrival instead - which is all upstream's cancel() achieves either: it exists
// so a stale card never lands on a field that has moved on.
void CancelWebPagePreview(Main::Session *session, const QString &link);

} // namespace CustomBackend
