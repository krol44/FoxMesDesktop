#!/usr/bin/env python3
"""Builds the body of a GitHub release.

The body is the static preamble in release-notes.md - platforms, signing,
how to verify the artifacts - followed by the changelog entries of the version
being released. Those entries live in changelog.txt and nowhere else: an entry
for the version is already mandatory, because Telegram/build/set_version.py
refuses to bump without exactly one, so the release notes read it instead of
duplicating it.

Run without arguments to print what the next release would publish:

    python3 Telegram/build/foxmes/make-release-notes.py
"""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
NOTES_PATH = ROOT / "Telegram" / "build" / "foxmes" / "release-notes.md"
CHANGELOG_PATH = ROOT / "changelog.txt"
VERSION_PATH = ROOT / "Telegram" / "build" / "version"

# The upstream parser of changelog.txt, so the format has exactly one reader.
sys.path.insert(0, str(ROOT / "Telegram" / "build"))
from changelog2appstream import parse_changelog  # noqa: E402

HEADING = "## Changes"


def version_names() -> list[str]:
    """The strings a changelog entry for the current version may start with.

    set_version.py accepts either the full version or, for a .0 patch, the
    short one - Telegram/build/version carries both, already computed.
    """
    wanted = ("AppVersionStr", "AppVersionStrSmall")
    names = []
    for line in VERSION_PATH.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] in wanted and parts[1] not in names:
            names.append(parts[1])
    if not names:
        raise SystemExit(f"No AppVersionStr in {VERSION_PATH}")
    return names


def changes_for(names: list[str]) -> list[str]:
    releases = parse_changelog(CHANGELOG_PATH)
    for version, _prerelease, _date, changes in releases:
        if version in names:
            if not changes:
                raise SystemExit(
                    f"Changelog entry for {version} lists no changes -"
                    f" add them to {CHANGELOG_PATH.name}")
            return changes
    raise SystemExit(
        f"No changelog entry for {' or '.join(names)} in"
        f" {CHANGELOG_PATH.name} - add one before releasing")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="write the notes here instead of stdout")
    args = parser.parse_args()

    preamble = NOTES_PATH.read_text(encoding="utf-8").rstrip("\n")
    changes = changes_for(version_names())
    lines = [preamble, "", HEADING, ""]
    lines.extend(f"- {change}" for change in changes)
    result = "\n".join(lines) + "\n"

    if args.output:
        args.output.write_text(result, encoding="utf-8")
    else:
        sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
