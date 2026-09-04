# FoxMes Desktop release scripts

These scripts are the only supported path for FoxMes release artifacts. They build `Release`, require Qt WebSockets, disable the upstream auto-updater and crash upload, and compile with `FOXMES_ALLOW_ENDPOINT_OVERRIDE=OFF`.

The Windows, macOS and Linux scripts produce the platform artifacts in `artifacts/`. `make-release-manifest.py` combines them with `version.json` and `SHA256SUMS`. `verify-source.py` audits user-facing package metadata before a build.

The Linux job pins appimagetool 1.9.1 (commit `8c8c91f762b412a19f4e8d2c4b35afb98f2d7c81`) with SHA256 `ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0`. Its type-2 runtime is pinned to release `20251108` (commit `dd6cebedcbddde9c82f89b011e8e1d40b6e43868`) with SHA256 `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`. Both URLs use immutable release tags; the release job never downloads a mutable `latest` or `continuous` asset.

Version 1.4.2 is unsigned. Windows SmartScreen and macOS Gatekeeper may warn on first launch. Users should verify `SHA256SUMS` and the GitHub build provenance before running an artifact. No Microsoft Store, Mac App Store, Snap, Flatpak or WinGet package is produced.
