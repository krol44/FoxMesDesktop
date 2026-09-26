#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="${ROOT_DIR}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/out}"
BUILD_CONFIG="${BUILD_CONFIG:-Release}"
# Profile: "dev" (default) runs against the local fxl-api,
# "prod" uses the production URL baked into the binary.
BUILD_ONLY=0
if [[ "${1:-}" == "--build-only" ]]; then
  BUILD_ONLY=1
  shift
fi
PROFILE="${1:-dev}"

if [[ ! -d "${PROJECT_DIR}" ]]; then
  echo "Project directory not found: ${PROJECT_DIR}" >&2
  exit 1
fi

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  echo "Missing ${BUILD_DIR}/CMakeCache.txt. Run ${ROOT_DIR}/Telegram/configure.sh first." >&2
  exit 1
fi

# Patches that live inside submodules have to be re-applied before every
# build: git submodule update discards them.
"${ROOT_DIR}/Telegram/patches/apply.sh"

# Architectures to build. Defaults to the host slice only: the fat
# x86_64;arm64 configuration compiles every translation unit twice, which is
# pure waste for a local dev run on Apple Silicon. Set ARCHS="x86_64;arm64"
# to produce a universal bundle.
ARCHS="${ARCHS:-$(uname -m)}"

# cmake_helpers defaults CMAKE_OSX_DEPLOYMENT_TARGET to 10.13, and Xcode 27
# refuses to build anything below 12.0. Keep the dev build on the same 13.0 as
# the release (Telegram/build/foxmes/build-macos.sh), so a local run compiles
# against the same minimum the shipped binary does.
DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET:-13.0}"

JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || printf '8')"
# Reconfigure with QT unset: cmake/external/qt/package.cmake writes an
# exported QT into the qt_requested cache entry with FORCE, so a QT that
# disagrees with the already configured tree fails the regeneration.
#
# FOXMES_LOCAL_BUILD makes this a separate application from the release in
# /Applications: FoxMes-local.app, ing.foxtail.FoxMes-local, its own working
# directory. Two ad-hoc signed bundles sharing one identifier cost the second
# one its microphone and camera access, silently.
env -u QT cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_OSX_ARCHITECTURES="${ARCHS}" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
  -DFOXMES_LOCAL_BUILD=ON
cmake --build "${BUILD_DIR}" --config "${BUILD_CONFIG}" --target Telegram -j"${JOBS}"

APP_BUNDLE="${BUILD_DIR}/${BUILD_CONFIG}/FoxMes-local.app"
if [[ ! -d "${APP_BUNDLE}" ]]; then
  echo "Application bundle not found: ${APP_BUNDLE}" >&2
  exit 1
fi

APP_BIN="${APP_BUNDLE}/Contents/MacOS/FoxMes-local"
if [[ ! -x "${APP_BIN}" ]]; then
  echo "Application binary not found inside ${APP_BUNDLE}" >&2
  exit 1
fi

if [[ "${BUILD_ONLY}" == "1" ]]; then
  echo "Build complete: ${APP_BUNDLE}"
  exit 0
fi

cd "${ROOT_DIR}"
if [[ "${PROFILE}" == "prod" ]]; then
  # Production URL is baked into the binary; no override.
  exec env -u FOXMES_URL "${APP_BIN}"
fi
# Dev profile: local fxl-api, FOXMES_URL can still be overridden manually.
exec env FOXMES_URL="${FOXMES_URL:-http://0.0.0.0:7034}" "${APP_BIN}"
