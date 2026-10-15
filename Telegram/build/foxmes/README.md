# FoxMes Desktop release scripts

These scripts are the only supported path for FoxMes release artifacts. They build `Release`, require Qt WebSockets, disable the upstream auto-updater and crash upload, and compile with `FOXMES_ALLOW_ENDPOINT_OVERRIDE=OFF`.

The Windows, macOS and Linux scripts produce the platform artifacts in `artifacts/`. `make-release-manifest.py` combines them with `version.json` and `SHA256SUMS`. `verify-source.py` audits user-facing package metadata before a build.

## Caches and when to warm them

A GitHub job runs for at most six hours, and a cold macOS or Windows job (all dependencies, then the app) may not fit. The dependency caches are keyed on `prepare.py` and `qt_version.py`, so they go cold after an upstream merge that touches either; the macOS ones also go cold when the runner image updates Xcode, because `prepare.py` keys every stage on the compiler and SDK. Linux builds its `centos_env` image whenever `centos_env/` changes, about two hours.

The Linux and macOS compiler caches (ccache) are saved only by runs on the default branch: a cache saved by a tag run is visible to that tag alone, so no later release could use it. On macOS ccache enters through the Xcode `CC` setting, since the Xcode generator ignores `CMAKE_<LANG>_COMPILER_LAUNCHER`, and every C-family compile gets `-Xclang -fno-pch-timestamp`, without which ccache will not cache anything that uses a precompiled header.

After a dependency change, release in three runs:

1. `workflow_dispatch` on `master` with `only_cache` checked: macOS and Windows build only the dependencies and save them; Linux builds and publishes the `centos_env` image.
2. `workflow_dispatch` on `master` without `only_cache`: a full build that checks every package and fills the compiler caches. It publishes nothing, because publishing needs a tag.
3. Push the `v*` tag.

For a release without dependency changes, step 2 alone keeps the compiler caches warm.

The Linux job pins appimagetool 1.9.1 (commit `8c8c91f762b412a19f4e8d2c4b35afb98f2d7c81`) with SHA256 `ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0`. Its type-2 runtime is pinned to release `20251108` (commit `dd6cebedcbddde9c82f89b011e8e1d40b6e43868`) with SHA256 `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`. Both URLs use immutable release tags; the release job never downloads a mutable `latest` or `continuous` asset.

Version 1.11.1 is unsigned. The macOS app is the exception: it carries FoxMes's self-signed certificate (`codesign.pem`, key in the `macos-signing` environment) so that macOS keeps its privacy permissions across updates, but it is not notarized. Windows SmartScreen and macOS Gatekeeper may warn on first launch. Users should verify `SHA256SUMS` and the GitHub build provenance before running an artifact. No Microsoft Store, Mac App Store, Snap, Flatpak or WinGet package is produced.
