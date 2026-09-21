#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import sys

VERSION = "1.7.2"
VERSION_CODE = 1007002
RELEASE_URL = (
    "https://github.com/krol44/FoxMesDesktop/releases/download/v" + VERSION
)
EXPECTED = {
    f"FoxMes-{VERSION}-windows-x64-setup.exe",
    f"FoxMes-{VERSION}-windows-x64-portable.zip",
    f"FoxMes-{VERSION}-macos-arm64.dmg",
    f"FoxMes-{VERSION}-linux-x86_64.AppImage",
    f"FoxMes-{VERSION}-linux-x86_64.tar.xz",
}
# The package each platform installs from, and the digest the client checks it
# against before running it. The admin only ever types a version number in the
# dashboard - the hashes are computed here, so they cannot be copied wrong.
PLATFORM_ASSETS = {
    "windows": f"FoxMes-{VERSION}-windows-x64-setup.exe",
    "macos": f"FoxMes-{VERSION}-macos-arm64.dmg",
    "linux_appimage": f"FoxMes-{VERSION}-linux-x86_64.AppImage",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    release_dir = Path(sys.argv[1]).resolve()
    names = {path.name for path in release_dir.iterdir() if path.is_file()}
    missing = EXPECTED - names
    if missing:
        raise SystemExit(f"Missing release artifacts: {sorted(missing)}")

    manifest = {"version_code": VERSION_CODE, "version": VERSION}
    for platform, name in PLATFORM_ASSETS.items():
        manifest[platform] = {
            "url": f"{RELEASE_URL}/{name}",
            "sha256": sha256(release_dir / name),
        }

    version_path = release_dir / "version.json"
    version_path.write_text(
        json.dumps(manifest, separators=(",", ":")),
        encoding="utf-8",
    )
    checksums = []
    for path in sorted(release_dir.iterdir(), key=lambda item: item.name):
        if not path.is_file() or path.name == "SHA256SUMS":
            continue
        checksums.append(f"{sha256(path)}  {path.name}")
    (release_dir / "SHA256SUMS").write_text(
        "\n".join(checksums) + "\n",
        encoding="ascii",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
