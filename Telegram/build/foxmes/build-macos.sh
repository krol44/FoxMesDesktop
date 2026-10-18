#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
source_root="${FOXMES_SOURCE_ROOT:-$(cd "$script_dir/../../.." && pwd)}"
artifact_root="${FOXMES_ARTIFACT_ROOT:-$source_root/artifacts/macos}"
libraries_root="${FOXMES_LIBRARIES_ROOT:-$(cd "$source_root/.." && pwd)/Libraries}"
version="1.12.0"

phase="${1:-all}"
case "$phase" in
  all|deps|app) ;;
  *) echo "usage: $(basename "$0") [all|deps|app]" >&2; exit 1 ;;
esac

prune_libraries() {
	if [ "${FOXMES_PRUNE_LIBRARIES:-}" != "1" ]; then
		return
	fi
	echo "pruning $libraries_root for caching"
	local qt_current old
	qt_current="$(python3 -c "import os, sys; sys.path.insert(0, sys.argv[1]); import qt_version; qt_version.resolve('arm64'); print(os.environ['QT'])" "$source_root/Telegram/build")"
	for old in "$libraries_root"/local/Qt-* "$libraries_root"/qt_* "$libraries_root"/cache_keys/qt_*; do
		[ -e "$old" ] || continue
		case "$(basename "$old")" in
			Qt-"$qt_current"|qt_"$qt_current") ;;
			*) echo "removing $old, the cache restored it for an older Qt"; rm -rf "$old" ;;
		esac
	done
	find "$libraries_root" \
		'(' '(' ! '(' -name '*.a' -o -name '*.h' -o -name '*.hpp' -o -name '*.inc' \
		-o -name '*.cmake' -o -name '*.pc' -o -path '*/include/*' -o -path '*/objects-*' \
		-o -path '*/cache_keys/*' -o -path '*/patches/*' -o -path '*/local/src/*' \
		-o '(' '(' -path "$libraries_root/tlottie/*" -o -path "$libraries_root/wallet-engine/*" ')' \
		! -path '*/target/*' ')' \
		-o -perm +111 ')' \
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

pkgconfig_stages=(
	xz:liblzma
	zlib:zlib
	mozjpeg:libjpeg
	opus:opus
	dav1d:dav1d
	openh264:openh264
	libavif:libavif
	libwebp:libwebp
	libheif:libheif
	libjxl:libjxl
	libvpx:vpx
	liblcms2:lcms2
	ffmpeg:libavcodec
	openal-soft:openal
	ada:ada
)

restore_pkgconfig() {
	local entry stage
	for entry in "${pkgconfig_stages[@]}"; do
		stage="${entry%%:*}"
		if [ -e "$libraries_root/cache_keys/$stage" ] \
			&& [ ! -e "$libraries_root/local/lib/pkgconfig/${entry#*:}.pc" ]; then
			echo "${entry#*:}.pc is missing, rebuilding the $stage stage"
			rm "$libraries_root/cache_keys/$stage"
		fi
	done
}

build_deps() {
	"$source_root/Telegram/patches/apply.sh"

	restore_pkgconfig

	"$source_root/Telegram/build/prepare/mac.sh" skip-debug skip-dump-syms silent

	prune_libraries
}

# The Xcode generator ignores CMAKE_<LANG>_COMPILER_LAUNCHER, so ccache goes
# in through the CC build setting, which Xcode uses for every C-family source.
# Clang stamps precompiled headers with their mtime unless told not to, and
# ccache refuses to cache any compile that uses such a header.
# Depend mode takes the header list from the -MMD file Xcode already asks
# for; otherwise every miss runs the preprocessor over the whole prefix
# header first, which made a cold build about 1.6 times slower.
ccache_args=()
setup_ccache() {
	if [ -z "${FOXMES_CCACHE_DIR:-}" ]; then
		return
	fi
	local clang clangxx launcher
	clang="$(xcrun -f clang)"
	clangxx="$(xcrun -f clang++)"
	launcher="$source_root/out/foxmes-ccache-clang"
	mkdir -p "$source_root/out" "$FOXMES_CCACHE_DIR"
	printf '#!/bin/sh\nexec ccache "%s" "$@"\n' "$clang" > "$launcher"
	chmod +x "$launcher"

	export CCACHE_DIR="$FOXMES_CCACHE_DIR"
	export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-5G}"
	export CCACHE_BASEDIR="$source_root"
	export CCACHE_COMPILERCHECK=content
	export CCACHE_DEPEND=true
	export CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime,clang_index_store
	ccache --zero-stats

	local no_pch_timestamp="-Xclang -fno-pch-timestamp"
	ccache_args=(
	  -D CMAKE_XCODE_ATTRIBUTE_CC="$launcher"
	  -D CMAKE_XCODE_ATTRIBUTE_LD="$clang"
	  -D CMAKE_XCODE_ATTRIBUTE_LDPLUSPLUS="$clangxx"
	  -D CMAKE_C_FLAGS="$no_pch_timestamp"
	  -D CMAKE_CXX_FLAGS="$no_pch_timestamp"
	  -D CMAKE_OBJC_FLAGS="$no_pch_timestamp"
	  -D CMAKE_OBJCXX_FLAGS="$no_pch_timestamp"
	)
}

build_app() {
	setup_ccache
	cd "$source_root/Telegram"
	./configure.sh \
	  -D CMAKE_CONFIGURATION_TYPES=Release \
	  -D CMAKE_OSX_ARCHITECTURES=arm64 \
	  -D CMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
	  -D CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
	  -D TDESKTOP_API_TEST=ON \
	  -D DESKTOP_APP_DISABLE_AUTOUPDATE=ON \
	  -D DESKTOP_APP_DISABLE_CRASH_REPORTS=ON \
	  -D FOXMES_ALLOW_ENDPOINT_OVERRIDE=OFF \
	  ${ccache_args[@]+"${ccache_args[@]}"}
	cmake --build "$source_root/out" --config Release --parallel
	if [ -n "${FOXMES_CCACHE_DIR:-}" ]; then
		ccache --show-stats
	fi

	local app="$source_root/out/Release/FoxMes.app"
	local binary="$app/Contents/MacOS/FoxMes"
	test -x "$binary"

	install -m 755 \
	  "$source_root/Telegram/build/foxmes/fox_update_macos.sh" \
	  "$app/Contents/Resources/fox_update_macos.sh"

	local cert="$source_root/Telegram/build/foxmes/codesign.pem"
	local identity=-
	if [ "${FOXMES_ADHOC_SIGN:-0}" != 1 ]; then
		if [ ! -f "$cert" ]; then
			echo "no signing certificate at $cert" >&2
			exit 1
		fi
		identity="$(openssl x509 -in "$cert" -noout -fingerprint -sha1 \
		  | sed 's/.*=//; s/://g' | tr '[:upper:]' '[:lower:]')"
	fi
	codesign --force \
	  --sign "$identity" \
	  --options runtime \
	  --entitlements "$source_root/Telegram/Telegram/Telegram.entitlements" \
	  "$app"
	codesign --verify --strict "$app"
	if [ "$identity" != - ]; then
		local expected="designated => identifier \"ing.foxtail.FoxMes\" and certificate root = H\"$identity\""
		local designated
		designated="$(codesign --display --requirements - "$app" 2>/dev/null \
		  | grep '^designated => ' || true)"
		if [ "$designated" != "$expected" ]; then
			echo "unexpected designated requirement: $designated" >&2
			echo "expected: $expected" >&2
			exit 1
		fi
	fi
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
	architectures="$(lipo -archs "$binary")"
	if [ "$architectures" != "arm64" ]; then
		echo "expected an arm64-only binary, got: $architectures" >&2
		exit 1
	fi
	test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$app/Contents/Info.plist")" = "ing.foxtail.FoxMes"
	test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app/Contents/Info.plist")" = "$version"
	test "$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$app/Contents/Info.plist")" = "13.0"
	test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleURLTypes:0:CFBundleURLSchemes:0' "$app/Contents/Info.plist")" = "foxmes"

	local stage="$source_root/out/foxmes-dmg"
	rm -rf "$stage"
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
