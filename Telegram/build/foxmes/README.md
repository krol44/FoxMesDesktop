# FoxMes Desktop release scripts

These scripts are the only supported path for FoxMes release artifacts. They build `Release`, require Qt WebSockets, disable the upstream auto-updater and crash upload, and compile with `FOXMES_ALLOW_ENDPOINT_OVERRIDE=OFF`.

The Windows, macOS and Linux scripts produce the platform artifacts in `artifacts/`. `make-release-manifest.py` combines them with `version.json` and `SHA256SUMS`. `verify-source.py` audits user-facing package metadata before a build.

## Caches and when to warm them

A GitHub job runs for at most six hours, and a cold build (dependencies, then the app) may not fit. The dependency caches are keyed on `prepare.py` and `qt_version.py`, so they go cold after an upstream merge that touches either; the macOS ones also go cold when the runner image updates Xcode, because `prepare.py` keys every stage on the compiler and SDK. Linux builds its `centos_env` image whenever `centos_env/` changes, about two hours.

Every platform runs as two jobs. `windows-cache`, `macos-cache` and `linux-cache` bring the dependencies (or the `centos_env` image) up to date, then compile the app into ccache without packaging it. Their compile step is given whatever is left of 330 minutes, so the cache is saved even when the compile runs out of time. The packaging jobs `windows`, `macos` and `linux` start after them even if they failed, restore that cache and get a fresh six hours, so a lost cache makes a release slower instead of failing it. A packaging job that fails saves its compiler cache too, so "Re-run failed jobs" continues from the objects already compiled.

A cache saved by a tag run is visible to that tag alone, so the compiler caches the next release starts from come from the default branch. After a tag run, `refresh-caches` starts a `workflow_dispatch` run on `master` with `only_cache` checked, which runs only the three `-cache` jobs. `cleanup-caches` deletes a tag's caches once its run succeeds and keeps only the newest compiler cache of each platform on `master`.

ccache enters the macOS build through the Xcode `CC` setting, since the Xcode generator ignores `CMAKE_<LANG>_COMPILER_LAUNCHER`, and every C-family compile gets `-Xclang -fno-pch-timestamp`, without which ccache will not cache anything that uses a precompiled header. ccache cannot cache MSVC compiles that use a precompiled header, and the sources rely on what that header includes, so whenever the Windows build uses ccache, `force-include-pch.cmake` (passed as `CMAKE_PROJECT_INCLUDE`) replaces each precompiled header with a `/FI` of the same header. The `centos_env` image sets `CCACHE_DISABLE` for its own stages, so `build-linux.sh` removes it for the app.

After a dependency change, run `workflow_dispatch` on `master` with `only_cache` checked before pushing the tag, so that the slow part happens outside the release.

The Linux job pins appimagetool 1.9.1 (commit `8c8c91f762b412a19f4e8d2c4b35afb98f2d7c81`) with SHA256 `ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0`. Its type-2 runtime is pinned to release `20251108` (commit `dd6cebedcbddde9c82f89b011e8e1d40b6e43868`) with SHA256 `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`. Both URLs use immutable release tags; the release job never downloads a mutable `latest` or `continuous` asset.

Version 1.13.0 is unsigned. The macOS app is the exception: it carries FoxMes's self-signed certificate (`codesign.pem`, key in the `macos-signing` environment) so that macOS keeps its privacy permissions across updates, but it is not notarized. Windows SmartScreen and macOS Gatekeeper may warn on first launch. Users should verify `SHA256SUMS` and the GitHub build provenance before running an artifact. No Microsoft Store, Mac App Store, Snap, Flatpak or WinGet package is produced.
