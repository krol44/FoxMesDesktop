#!/usr/bin/env bash

set -euo pipefail

patches_dir="$(cd "$(dirname "$0")" && pwd)"
source_root="$(cd "$patches_dir/../.." && pwd)"

apply_one() {
	local patch="$patches_dir/$1"
	local target="$source_root/$2"

	if [[ ! -f "$patch" ]]; then
		echo "patches: missing $patch" >&2
		return 1
	elif [[ ! -d "$target" ]]; then
		echo "patches: $2 is not checked out, run git submodule update" >&2
		return 1
	fi

	if git -C "$target" apply --reverse --check "$patch" 2>/dev/null; then
		echo "patches: $1 already applied"
	elif git -C "$target" apply --check "$patch" 2>/dev/null; then
		git -C "$target" apply "$patch"
		echo "patches: $1 applied to $2"
	else
		echo "patches: $1 does not apply to $2." >&2
		echo "  Upstream most likely changed the surrounding code." >&2
		echo "  Re-create the patch against the current submodule commit," >&2
		echo "  or drop it if upstream fixed the same thing." >&2
		return 1
	fi
}

apply_one lib_ui-animated-icon-webm.patch Telegram/lib_ui
apply_one lib_base-screen-capture-settings-mac.patch Telegram/lib_base
apply_one lib_webrtc-screen-capture-permission-mac.patch Telegram/lib_webrtc
apply_one tgcalls-screen-share-quality.patch Telegram/ThirdParty/tgcalls
apply_one tgcalls-call-recovery.patch Telegram/ThirdParty/tgcalls
apply_one cmake-bzip2-stub.patch cmake
apply_one cmake-gcc-restrict-warning.patch cmake
apply_one cmake-qt-win-dep-paths.patch cmake
