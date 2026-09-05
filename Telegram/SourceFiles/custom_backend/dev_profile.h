#pragma once

#include <QString>

#include <cstdlib>

namespace CustomBackend {

// The suffix every account store carries, so that clients which know nothing
// of each other's logins never write over one another. It is appended to the
// working directory (Core::Launcher::validateCustomWorkingDir) and to the
// QSettings store that keeps the bearer (custom_backend/token_store.h).
//
// Two runs need it, for two different reasons:
//
// - dev-client.sh points the client at the local fxl-api through FOXMES_URL,
//   see EnvBaseUrl() in native_runtime.cpp - the same variable is the single
//   marker of a dev run. The two backends issue their own bearers and know
//   nothing of each other's user ids, so whichever client started last used to
//   overwrite the other one's login.
//
// - a local build running against production (prod-client.sh, dev-client.sh
//   prod) is a different application from the release installed in
//   /Applications - see FOXMES_LOCAL_BUILD in Telegram/cmake/foxmes.cmake -
//   and the two can be running at the same time, because the single-instance
//   lock is keyed on the executable path.
//
// A dev run keeps the plain "-dev" it has always used, and a local build
// running against production gets "-local".
//
// Both branches key off FOXMES_ALLOW_ENDPOINT_OVERRIDE, because that flag is
// already the marker of a local build: every release entry point in
// Telegram/build/foxmes/ passes it OFF, and only dev-client.sh and
// prod-client.sh leave it on - the same two scripts that configure with
// FOXMES_LOCAL_BUILD. That option carries the CMake half of the split, the
// bundle name and identifier; asking for a compile definition on top of it
// would rebuild every translation unit of the app to learn what this one
// already knows.
//
// Empty in a release build: the override is off there, so neither a stray
// FOXMES_URL in the environment nor this suffix can move the user's data.
[[nodiscard]] inline QString ProfileSuffix() {
#if FOXMES_ALLOW_ENDPOINT_OVERRIDE
	const auto url = std::getenv("FOXMES_URL");
	if (url && !QString::fromUtf8(url).trimmed().isEmpty()) {
		return u"-dev"_q;
	}
	return u"-local"_q;
#else // FOXMES_ALLOW_ENDPOINT_OVERRIDE
	return QString();
#endif // !FOXMES_ALLOW_ENDPOINT_OVERRIDE
}

} // namespace CustomBackend
