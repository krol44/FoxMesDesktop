#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="${ROOT_DIR}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/out}"
BUILD_CONFIG="${BUILD_CONFIG:-Release}"
BUILD_ONLY=0
if [[ "${1:-}" == "--build-only" ]]; then
  BUILD_ONLY=1
  shift
fi
PROFILE="${1:-dev}"

APP_BUNDLE="${BUILD_DIR}/${BUILD_CONFIG}/FoxMes-local.app"
APP_BIN="${APP_BUNDLE}/Contents/MacOS/FoxMes-local"
DEV_URL="${FOXMES_URL:-http://0.0.0.0:7034}"

launch() {
  local open_args=(-n -W)
  if tty -s; then
    open_args+=(--stdout "$(tty)" --stderr "$(tty)")
  fi
  open "${open_args[@]}" "$@"
}

if [[ "${PROFILE}" == "second" ]]; then
  if [[ ! -x "${APP_BIN}" ]]; then
    echo "Application binary not found: ${APP_BIN}. Run ${ROOT_DIR}/dev-client.sh first." >&2
    exit 1
  fi
  SECOND_WORKDIR="${SECOND_WORKDIR:-${HOME}/Library/Application Support/FoxMes-dev2}"
  mkdir -p "${SECOND_WORKDIR}"
  launch --env FOXMES_URL="${DEV_URL}" "${APP_BUNDLE}" \
    --args -workdir "${SECOND_WORKDIR}" "${@:2}"
  exit
fi

if [[ ! -d "${PROJECT_DIR}" ]]; then
  echo "Project directory not found: ${PROJECT_DIR}" >&2
  exit 1
fi

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  echo "Missing ${BUILD_DIR}/CMakeCache.txt. Run ${ROOT_DIR}/Telegram/configure.sh first." >&2
  exit 1
fi

"${ROOT_DIR}/Telegram/patches/apply.sh"

ARCHS="${ARCHS:-$(uname -m)}"

DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET:-13.0}"

JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || printf '8')"

if [[ -z "${FOXMES_SIGN_IDENTITY+x}" ]]; then
  FOXMES_SIGN_IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
    | awk -F'"' '/"Apple Development: /{print $2; exit}' || true)"
fi
SIGN_TEAM=""
if [[ -n "${FOXMES_SIGN_IDENTITY}" ]]; then
  SIGN_TEAM="$(security find-certificate -c "${FOXMES_SIGN_IDENTITY}" -p 2>/dev/null \
    | openssl x509 -noout -subject 2>/dev/null \
    | sed -nE 's/.*OU *= *([A-Z0-9]+).*/\1/p' || true)"
  echo "Signing with: ${FOXMES_SIGN_IDENTITY} (team ${SIGN_TEAM:-?})"
else
  echo "No Apple Development identity found; signing ad-hoc (TCC grants reset on every rebuild)."
fi

env -u QT cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_OSX_ARCHITECTURES="${ARCHS}" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
  -DFOXMES_LOCAL_BUILD=ON \
  -DFOXMES_LOCAL_SIGN_IDENTITY="${FOXMES_SIGN_IDENTITY}" \
  -DFOXMES_LOCAL_SIGN_TEAM="${SIGN_TEAM}"
cmake --build "${BUILD_DIR}" --config "${BUILD_CONFIG}" --target Telegram -j"${JOBS}"

if [[ ! -d "${APP_BUNDLE}" ]]; then
  echo "Application bundle not found: ${APP_BUNDLE}" >&2
  exit 1
fi

if [[ ! -x "${APP_BIN}" ]]; then
  echo "Application binary not found inside ${APP_BUNDLE}" >&2
  exit 1
fi

if [[ "${BUILD_ONLY}" == "1" ]]; then
  echo "Build complete: ${APP_BUNDLE}"
  exit 0
fi

if [[ "${PROFILE}" == "prod" ]]; then
  launch "${APP_BUNDLE}"
  exit
fi
launch --env FOXMES_URL="${DEV_URL}" "${APP_BUNDLE}"
