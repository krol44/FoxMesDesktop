#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import sys

VERSION = "1.4.1"
VERSION_CODE = 1004001
EXPECTED = {
    f"FoxMes-{VERSION}-windows-x64-setup.exe",
    f"FoxMes-{VERSION}-windows-x64-portable.zip",
    f"FoxMes-{VERSION}-macos-arm64.dmg",
    f"FoxMes-{VERSION}-linux-x86_64.AppImage",
    f"FoxMes-{VERSION}-linux-x86_64.tar.xz",
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

    version_path = release_dir / "version.json"
    version_path.write_text(
        json.dumps(
            {"version_code": VERSION_CODE, "version": VERSION},
            separators=(",", ":"),
        ),
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
