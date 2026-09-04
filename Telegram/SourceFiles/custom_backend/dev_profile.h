#pragma once

#include <QString>

#include <cstdlib>

namespace CustomBackend {

// dev-client.sh points the client at the local fxl-api through FOXMES_URL,
// see EnvBaseUrl() in native_runtime.cpp - the same variable is the single
// marker of a dev run.
//
// A dev run must not share state with the production client: the two backends
// issue their own bearers and know nothing of each other's user ids, so
// whichever client started last used to overwrite the other one's login.
// Everything that stores an account - the working directory and the QSettings
// store - carries this suffix in a dev run, so the two never meet.
//
// Empty in a release build: FOXMES_ALLOW_ENDPOINT_OVERRIDE is off there and
// a stray FOXMES_URL in the environment cannot move the user's data.
[[nodiscard]] inline QString DevProfileSuffix() {
#if FOXMES_ALLOW_ENDPOINT_OVERRIDE
	const auto url = std::getenv("FOXMES_URL");
	if (url && !QString::fromUtf8(url).trimmed().isEmpty()) {
		return u"-dev"_q;
	}
#endif // FOXMES_ALLOW_ENDPOINT_OVERRIDE
	return QString();
}

} // namespace CustomBackend
