#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
source_root="${FOXMES_SOURCE_ROOT:-$(cd "$script_dir/../../.." && pwd)}"
artifact_root="${FOXMES_ARTIFACT_ROOT:-$source_root/artifacts/linux}"
version="1.4.3"
image_tag="foxmes:centos_env"
appdir="$source_root/out/FoxMes.AppDir"
tools="$source_root/out/foxmes-tools"

"$source_root/Telegram/patches/apply.sh"

# Building this image costs about two hours, 101 minutes of which is the single
# Qt layer, and it only changes when centos_env/ changes. FOXMES_CENTOS_ENV_IMAGE
# names one exact content-addressed tag, so a miss can only mean "nobody has
# built this version yet" - never "the wrong image".
#
# This build is self-healing rather than dependent on somebody remembering: on a
# miss it builds the image and publishes it, so the next release pulls it in
# seconds. That is why the tag is a content hash - publishing from here can never
# overwrite an image someone else's build is using.
if [ -n "${FOXMES_CENTOS_ENV_IMAGE:-}" ] && docker pull "$FOXMES_CENTOS_ENV_IMAGE"
then
  echo "using the published image $FOXMES_CENTOS_ENV_IMAGE"
  docker tag "$FOXMES_CENTOS_ENV_IMAGE" "$image_tag"
else
  publish_after_build=
  if [ -n "${FOXMES_CENTOS_ENV_IMAGE:-}" ]; then
    echo "$FOXMES_CENTOS_ENV_IMAGE is not published yet, building it here" >&2
    publish_after_build=1
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
      {
        echo "### centos_env image built in this job"
        echo
        echo "\`$FOXMES_CENTOS_ENV_IMAGE\` was not published, so this run built"
        echo "it - about two hours - and pushed it. Later releases pull it in"
        echo "seconds until something under centos_env/ changes the tag again."
      } >> "$GITHUB_STEP_SUMMARY"
    fi
  fi
  # gen_dockerfile defaults both of these on, which bakes -g -gdwarf64 -gz and
  # -flto=auto -ffat-lto-objects into CFLAGS for every library in the image and
  # for the app built against it. -ffat-lto-objects makes the compiler emit both
  # object code and LTO IR, so every translation unit is code-generated twice,
  # and the debug info is dead weight here because the packages ship with
  # DESKTOP_APP_DISABLE_CRASH_REPORTS=ON. Upstream's own CI clears both the same
  # way. checkEnv() treats a defined-but-empty variable as false. The publishing
  # workflow must clear them identically or the image will not match this build.
  export DEBUG=
  export LTO=
  "$source_root/Telegram/build/prepare/linux.sh"
  docker tag tdesktop:centos_env "$image_tag"

  # Push what we just built so the next run does not repeat it. Never fatal: a
  # run without registry credentials - a local build, a fork - should still
  # produce packages, it just does not get to share the image.
  if [ -n "$publish_after_build" ]; then
    docker tag tdesktop:centos_env "$FOXMES_CENTOS_ENV_IMAGE"
    # Retried because the first attempt has been seen to upload most layers and
    # then fail with "unknown blob" - a registry-side answer, not a permissions
    # one, and one a second attempt can get past now that those layers are
    # already stored. Bounded at three: if it is not transient, more tries only
    # add minutes to a build that is otherwise fine.
    pushed=
    for attempt in 1 2 3; do
      if docker push "$FOXMES_CENTOS_ENV_IMAGE"; then
        pushed=1
        echo "published $FOXMES_CENTOS_ENV_IMAGE"
        break
      fi
      echo "push attempt $attempt failed" >&2
      [ "$attempt" = 3 ] || sleep 15
    done
    if [ -z "$pushed" ]; then
      echo "could not push $FOXMES_CENTOS_ENV_IMAGE, continuing" >&2
      if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
        {
          echo "### centos_env image could not be published"
          echo
          echo "It was built here and the packages are fine, but the push to"
          echo "\`$FOXMES_CENTOS_ENV_IMAGE\` failed three times, so the next"
          echo "Linux run will build it again - about two hours."
        } >> "$GITHUB_STEP_SUMMARY"
      fi
    fi
  fi
fi

# The image sets CCACHE_DIR=/var/cache/ccache and compiles through ccache, but
# nothing was mounted there for the app build, so every hit went into the
# container's writable layer and --rm threw it away - the whole app was
# recompiled from scratch on every run. The Dockerfile keeps its own ccache
# across image builds with BuildKit cache mounts; this gives the app the same
# treatment on a runner whose disk survives between jobs. Harmless on a
# throwaway runner, where it simply starts empty.
ccache_dir="${FOXMES_CCACHE_DIR:-$HOME/.cache/foxmes-ccache}"
mkdir -p "$ccache_dir"

docker run --rm \
  -u "$(id -u):$(id -g)" \
  -v "$source_root:/usr/src/tdesktop" \
  -v "$ccache_dir:/var/cache/ccache" \
  -e CCACHE_MAXSIZE=2G \
  -e CONFIG=Release \
  "$image_tag" \
  /usr/src/tdesktop/Telegram/build/docker/centos_env/build.sh \
  -D CMAKE_CONFIGURATION_TYPES=Release \
  -D CMAKE_INSTALL_PREFIX=/usr \
  -D TDESKTOP_API_TEST=ON \
  -D DESKTOP_APP_DISABLE_AUTOUPDATE=ON \
  -D DESKTOP_APP_DISABLE_CRASH_REPORTS=ON \
  -D FOXMES_ALLOW_ENDPOINT_OVERRIDE=OFF

rm -rf "$appdir"
docker run --rm \
  -u "$(id -u):$(id -g)" \
  -v "$source_root:/usr/src/tdesktop" \
  -e DESTDIR=/usr/src/tdesktop/out/FoxMes.AppDir \
  "$image_tag" \
  cmake --install /usr/src/tdesktop/out --config Release --prefix /usr

test -x "$appdir/usr/bin/FoxMes"
# Emptied, not just created: on a runner that keeps the workspace between
# runs, a package left by an earlier build of a different version would still
# be sitting here and the upload step ships everything in this directory.
# out/foxmes-tools is deliberately kept - each tool there is verified by digest
# before it is reused.
rm -rf "$artifact_root"
mkdir -p "$artifact_root" "$tools"
ln -s usr/bin/FoxMes "$appdir/AppRun"
cp "$appdir/usr/share/applications/ru.fxl.foxMes.desktop" "$appdir/ru.fxl.foxMes.desktop"
cp "$appdir/usr/share/icons/hicolor/256x256/apps/ru.fxl.foxMes.png" "$appdir/ru.fxl.foxMes.png"

tar -C "$appdir" -cJf "$artifact_root/FoxMes-$version-linux-x86_64.tar.xz" .

appimagetool_url="https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
appimagetool_sha256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
runtime_url="https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
runtime_sha256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"

# Both tools are pinned by digest, so a copy left in out/foxmes-tools by an
# earlier run - restored from the workflow cache, say - is byte-identical to
# what the download would produce and is taken as is. A file that fails the
# digest is re-fetched rather than trusted, and the download is verified the
# same way, so a corrupted cache entry costs a download, never a bad package.
fetch_tool() {
  local url="$1" sha="$2" target="$3"
  if [ -f "$target" ] && echo "$sha  $target" | sha256sum --check --status --strict
  then
    echo "reusing $target"
    return
  fi
  curl --fail --location --proto '=https' --tlsv1.2 "$url" -o "$target"
  echo "$sha  $target" | sha256sum --check --strict
}

fetch_tool "$appimagetool_url" "$appimagetool_sha256" "$tools/appimagetool"
fetch_tool "$runtime_url" "$runtime_sha256" "$tools/runtime-x86_64"
chmod +x "$tools/appimagetool"

ARCH=x86_64 "$tools/appimagetool" --appimage-extract-and-run \
  --runtime-file "$tools/runtime-x86_64" \
  "$appdir" \
  "$artifact_root/FoxMes-$version-linux-x86_64.AppImage"
