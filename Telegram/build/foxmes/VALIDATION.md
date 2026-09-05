# FoxMes Desktop 1.4.4 validation

The release workflow performs source metadata checks, exact tag/version matching, Release-only builds, artifact naming, SHA256 verification, Linux desktop/AppStream/AppImage checks, arm64 Mach-O and bundle-ID checks, Windows version-resource/install/uninstall/URL-scheme checks, and GitHub provenance attestation.

Before publishing `v1.4.4`, complete these manual checks on the produced artifacts:

- Windows 10 1809+ x64: install and portable launch, shortcuts, notifications, autostart, `foxmes://open`, uninstall, and coexistence with Telegram Desktop.
- macOS 13+ on Apple silicon: mount the DMG, launch the unsigned app, verify `foxmes://open`, notifications, autostart and coexistence. Intel Macs are out of scope: the package is arm64-only.
- Ubuntu 22.04 and 24.04 x86_64: launch both AppImage and tar package, verify desktop/DBus activation, notifications, autostart and coexistence.
- On every platform: device pairing, QSettings session restoration, personal chat, Saved Messages, history pagination, send/receive, file transfer and WebSocket reconnect.
- Update checker: newer/equal/older version codes, malformed and oversized manifests, HTTP errors, non-GitHub or insecure redirects, and confirmation that every update action opens only `https://github.com/krol44/FoxMesDesktop/releases/latest`.
- Deep links: `foxmes://open` launches or focuses exactly one FoxMes instance; unknown FoxMes routes have no effect; Telegram Desktop remains the owner of `tg://`.
- Endpoint isolation: a package build launched with `FOXMES_URL=http://attacker.invalid` still uses `https://api-fox-mes.fxl.ru` and does not disable TLS verification.

Version 1.4.4 is unsigned. Record SmartScreen/Gatekeeper behavior in the release checklist and verify every downloaded artifact against both `SHA256SUMS` and GitHub provenance.
