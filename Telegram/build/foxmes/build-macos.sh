#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
source_root="${FOXMES_SOURCE_ROOT:-$(cd "$script_dir/../../.." && pwd)}"
artifact_root="${FOXMES_ARTIFACT_ROOT:-$source_root/artifacts/macos}"
libraries_root="${FOXMES_LIBRARIES_ROOT:-$(cd "$source_root/.." && pwd)/Libraries}"
version="1.4.3"

# Two phases, because CI caches the dependency tree between them and needs a
# seam to hang the cache save on. No argument runs both, so a local build is
# unchanged: ./build-macos.sh
phase="${1:-all}"
case "$phase" in
  all|deps|app) ;;
  *) echo "usage: $(basename "$0") [all|deps|app]" >&2; exit 1 ;;
esac

# Reduces the dependency tree to what the app actually links against, so that it
# fits in a cache. Opt-in, because it is destructive and nobody wants it running
# over a working tree they will build in again tomorrow.
#
# Two rules, in order. The first is upstream tdesktop's: keep link inputs,
# headers, CMake files, the prepare.py stage markers and the patches those stage
# keys are hashed from; delete everything else. Upstream runs this in the same
# job right before building the app, so the app provably builds against what
# survives - the filter is proven by their CI rather than by argument.
#
# The second rule is ours. Qt's build tree stages a second copy of every static
# library that `cmake --install` already placed in local/Qt-$QT/lib, and the app
# links the installed copy (cmake/external/qt/package.cmake). Dropping the
# duplicates saves about 8 GB and nothing reads them afterwards.
#
# What must not happen is a stage directory vanishing: prepare.py skips a stage
# only while `Libraries/<stage>/` still exists (prepare.py:199), and a missing
# one means `rm -rf` plus a full rebuild. `-o -empty` deletes empty directories
# too, so the check at the end is not paranoia - it is the failure this step can
# actually produce, and it would otherwise show up as an unexplained hour of
# rebuilding on every future run.
prune_libraries() {
	if [ "${FOXMES_PRUNE_LIBRARIES:-}" != "1" ]; then
		return
	fi
	echo "pruning $libraries_root for caching"
	find "$libraries_root" \
		'(' '(' ! '(' -name '*.a' -o -name '*.h' -o -name '*.hpp' -o -name '*.inc' \
		-o -name '*.cmake' -o -path '*/include/*' -o -path '*/objects-*' \
		-o -path '*/cache_keys/*' -o -path '*/patches/*' -o -perm +111 ')' \
		-type f ')' -o -empty ')' -delete
	find "$libraries_root"/qt_* -name '*.a' -delete 2>/dev/null || true

	local missing=0
	local marker name
	for marker in "$libraries_root"/cache_keys/*; do
		[ -e "$marker" ] || continue
		name="$(basename "$marker")"
		if [ ! -d "$libraries_root/$name" ]; then
			echo "prune deleted the whole $name stage directory" >&2
			missing=1
		fi
	done
	if [ "$missing" = 1 ]; then
		echo "prepare.py would rebuild those stages from scratch on every run;" >&2
		echo "add a -path term keeping one of their files to the filter above." >&2
		exit 1
	fi
}

build_deps() {
	"$source_root/Telegram/patches/apply.sh"

	# skip-debug, not skip-release: the app below is built --config Release, so
	# the dependencies have to be Release too. skip-release would build only
	# Debug ones and the link would fail on the missing Release libraries;
	# passing neither would build both and roughly double a job that already
	# runs for hours.
	"$source_root/Telegram/build/prepare/mac.sh" skip-debug skip-dump-syms silent

	prune_libraries
}

build_app() {
	cd "$source_root/Telegram"
	# No CMAKE_COMPILE_WARNING_AS_ERROR here, and none in the Linux and Windows
	# scripts either: a warning gate belongs where it gives feedback in minutes,
	# not at the end of an hours-long release build where it blocks the release
	# itself. Worth restoring as a separate check workflow on pushes to master.
	./configure.sh \
	  -D CMAKE_CONFIGURATION_TYPES=Release \
	  -D CMAKE_OSX_ARCHITECTURES=arm64 \
	  -D CMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
	  -D CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
	  -D TDESKTOP_API_TEST=ON \
	  -D DESKTOP_APP_DISABLE_AUTOUPDATE=ON \
	  -D DESKTOP_APP_DISABLE_CRASH_REPORTS=ON \
	  -D FOXMES_ALLOW_ENDPOINT_OVERRIDE=OFF
	cmake --build "$source_root/out" --config Release --parallel

	local app="$source_root/out/Release/FoxMes.app"
	local binary="$app/Contents/MacOS/FoxMes"
	test -x "$binary"

	# CODE_SIGNING_ALLOWED=NO above skips Xcode's signing phase, and with it
	# the only step that applies Telegram.entitlements. The linker still ad-hoc
	# signs the executable, so the app runs on arm64 and looks signed - but it
	# carries no entitlements and no hardened runtime, and then recording a
	# voice or video message dies the moment it starts: TCC hands out no
	# microphone or camera access. Signing here restores exactly what a local
	# ./dev-client.sh build gets from Xcode.
	#
	# --sign - is ad-hoc: no certificate, no key, no Apple developer account.
	# The cost is that the identity is the code hash, so it changes with every
	# release and macOS asks for microphone access again after each update. A
	# Developer ID signature plus notarization is what would make the grant
	# stick, and that does need a paid account - this does not.
	codesign --force \
	  --sign - \
	  --options runtime \
	  --entitlements "$source_root/Telegram/Telegram/Telegram.entitlements" \
	  "$app"
	codesign --verify --strict "$app"
	# The signature can succeed and still carry nothing: an entitlements file
	# that failed to parse, or a signing step quietly skipped by a future
	# refactor of the flags above, both end here rather than in a release
	# nobody can record with.
	for entitlement in \
	  com.apple.security.device.audio-input \
	  com.apple.security.device.camera
	do
		if ! codesign --display --entitlements - "$app" 2>/dev/null \
		  | grep -q "$entitlement"; then
			echo "signed bundle is missing $entitlement" >&2
			exit 1
		fi
	done
	# lipo takes the input file first: "lipo -verify_arch arm64 <file>" makes it
	# read the path as another architecture name and fail with a usage error.
	# -verify_arch is dropped entirely because it passes on a universal binary
	# too, so it could not catch a regression back to a fat build; comparing the
	# full architecture list is the check that actually says "arm64 and nothing
	# else", and it reports what it found when it fails.
	architectures="$(lipo -archs "$binary")"
	if [ "$architectures" != "arm64" ]; then
		echo "expected an arm64-only binary, got: $architectures" >&2
		exit 1
	fi
	test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$app/Contents/Info.plist")" = "ru.fxl.foxMes"
	test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app/Contents/Info.plist")" = "$version"
	test "$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$app/Contents/Info.plist")" = "13.0"
	test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleURLTypes:0:CFBundleURLSchemes:0' "$app/Contents/Info.plist")" = "foxmes"

	local stage="$source_root/out/foxmes-dmg"
	rm -rf "$stage"
	# Emptied for the same reason the Linux script empties its own: a stale
	# package from an earlier version would otherwise be uploaded as part of
	# this release on any runner that keeps its workspace.
	rm -rf "$artifact_root"
	mkdir -p "$stage" "$artifact_root"
	cp -R "$app" "$stage/FoxMes.app"
	ln -s /Applications "$stage/Applications"
	hdiutil create \
	  -volname "FoxMes Desktop" \
	  -srcfolder "$stage" \
	  -ov \
	  -format UDZO \
	  "$artifact_root/FoxMes-$version-macos-arm64.dmg"
}

case "$phase" in
  deps) build_deps ;;
  app)  build_app ;;
  all)  build_deps; build_app ;;
esac
