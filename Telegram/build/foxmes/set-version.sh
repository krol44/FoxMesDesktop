#!/usr/bin/env bash
#
# Bumps the FoxMes version everywhere it's hardcoded, or checks that it's
# still consistent everywhere. Run through `make version` / `make version-check`
# from the repo root - see the root Makefile.
#
# Canonical source: Telegram/build/version, patched by the upstream
# Telegram/build/set_version.py (also patches core/version.h, Telegram.rc,
# Updater.rc, AppxManifest.xml, and requires a matching changelog.txt entry).
# Everything below is FoxMes-specific duplication that set_version.py does
# not know about and that has to be kept in sync by hand otherwise.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
VERSION_FILE="$ROOT/Telegram/build/version"

usage() {
  echo "Usage:"
  echo "  $(basename "$0") X.Y.Z     Bump the FoxMes version to X.Y.Z everywhere"
  echo "  $(basename "$0") --check   Verify all hardcoded version literals match Telegram/build/version"
  exit 1
}

[ $# -eq 1 ] || usage

current_version() {
  awk '$1 == "AppVersionStr" { print $2 }' "$VERSION_FILE"
}

current_code() {
  awk '$1 == "AppVersion" { print $2 }' "$VERSION_FILE"
}

PATCH_FILES=()
PATCH_OLD=()
PATCH_NEW=()

add_patch() {
  PATCH_FILES+=("$1")
  PATCH_OLD+=("$2")
  PATCH_NEW+=("$3")
}

# Builds the list of (file, old-literal, new-literal) triples that duplicate
# the version outside Telegram/build/version. Pass the same value twice
# (ov=nv, oc=nc) to build a "does the current version literal exist" check list.
build_patch_list() {
  local ov="$1" oc="$2" nv="$3" nc="$4"
  PATCH_FILES=(); PATCH_OLD=(); PATCH_NEW=()

  add_patch "$ROOT/.github/workflows/foxmes-release.yml" \
    "FOXMES_VERSION: $ov" "FOXMES_VERSION: $nv"
  add_patch "$ROOT/.github/workflows/foxmes-release.yml" \
    "FOXMES_VERSION_CODE: '$oc'" "FOXMES_VERSION_CODE: '$nc'"
  add_patch "$ROOT/.github/workflows/foxmes-release.yml" \
    "artifacts/linux/FoxMes-$ov-linux-x86_64.AppImage" "artifacts/linux/FoxMes-$nv-linux-x86_64.AppImage"

  add_patch "$ROOT/Telegram/build/foxmes/verify-source.py" \
    "!= \"$oc\"" "!= \"$nc\""
  add_patch "$ROOT/Telegram/build/foxmes/verify-source.py" \
    "!= \"$ov\"" "!= \"$nv\""

  add_patch "$ROOT/Telegram/build/foxmes/make-release-manifest.py" \
    "VERSION = \"$ov\"" "VERSION = \"$nv\""
  add_patch "$ROOT/Telegram/build/foxmes/make-release-manifest.py" \
    "VERSION_CODE = $oc" "VERSION_CODE = $nc"

  add_patch "$ROOT/Telegram/build/foxmes/build-macos.sh" \
    "version=\"$ov\"" "version=\"$nv\""
  add_patch "$ROOT/Telegram/build/foxmes/build-linux.sh" \
    "version=\"$ov\"" "version=\"$nv\""

  add_patch "$ROOT/Telegram/build/foxmes/build-windows.ps1" \
    "\$version = \"$ov\"" "\$version = \"$nv\""
  add_patch "$ROOT/Telegram/build/foxmes/build-windows.ps1" \
    "-notlike \"$ov*\"" "-notlike \"$nv*\""
  add_patch "$ROOT/Telegram/build/foxmes/build-windows.ps1" \
    "The version $ov installer must be unsigned." "The version $nv installer must be unsigned."

  add_patch "$ROOT/Telegram/build/foxmes/foxmes.iss" \
    "MyAppVersion \"$ov\"" "MyAppVersion \"$nv\""
  add_patch "$ROOT/Telegram/build/foxmes/foxmes.iss" \
    "OutputBaseFilename=FoxMes-$ov-windows-x64-setup" "OutputBaseFilename=FoxMes-$nv-windows-x64-setup"

  add_patch "$ROOT/Telegram/build/foxmes/README.md" \
    "Version $ov is unsigned." "Version $nv is unsigned."

  add_patch "$ROOT/Telegram/build/foxmes/VALIDATION.md" \
    "# FoxMes Desktop $ov validation" "# FoxMes Desktop $nv validation"
  add_patch "$ROOT/Telegram/build/foxmes/VALIDATION.md" \
    "\`v$ov\`" "\`v$nv\`"
  add_patch "$ROOT/Telegram/build/foxmes/VALIDATION.md" \
    "Version $ov is unsigned." "Version $nv is unsigned."

  add_patch "$ROOT/Telegram/build/foxmes/release-notes.md" \
    "# FoxMes Desktop $ov" "# FoxMes Desktop $nv"
}

apply_patches() {
  local i file old new
  for i in "${!PATCH_FILES[@]}"; do
    file="${PATCH_FILES[$i]}"
    old="${PATCH_OLD[$i]}"
    new="${PATCH_NEW[$i]}"
    if ! grep -qF -- "$old" "$file"; then
      echo "ERROR: expected text not found in ${file#"$ROOT"/}:" >&2
      echo "  $old" >&2
      echo "The file changed since set-version.sh was written - update its patch list." >&2
      exit 1
    fi
    OLD_LIT="$old" NEW_LIT="$new" perl -pi -e 's/\Q$ENV{OLD_LIT}\E/$ENV{NEW_LIT}/g' "$file"
    echo "  patched ${file#"$ROOT"/}"
  done
}

check_patches() {
  local i file new failed=0
  for i in "${!PATCH_FILES[@]}"; do
    file="${PATCH_FILES[$i]}"
    new="${PATCH_NEW[$i]}"
    if grep -qF -- "$new" "$file"; then
      printf "  OK    %s\n" "${file#"$ROOT"/}"
    else
      printf "  FAIL  %s: expected %s\n" "${file#"$ROOT"/}" "$new"
      failed=1
    fi
  done
  return $failed
}

if [ "$1" = "--check" ]; then
  CUR="$(current_version)"
  CUR_CODE="$(current_code)"
  echo "Canonical version (Telegram/build/version): $CUR ($CUR_CODE)"
  echo
  build_patch_list "$CUR" "$CUR_CODE" "$CUR" "$CUR_CODE"
  if check_patches; then
    echo
    echo "All FoxMes version literals match."
    exit 0
  else
    echo
    echo "Some files are out of sync with Telegram/build/version - fix them or re-run the bump." >&2
    exit 1
  fi
fi

NEW="$1"
[[ "$NEW" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "Version must look like X.Y.Z (e.g. 1.1.0), got: $NEW" >&2
  exit 1
}

IFS='.' read -r MAJOR MINOR PATCH <<< "$NEW"
NEW_CODE=$((10#$MAJOR * 1000000 + 10#$MINOR * 1000 + 10#$PATCH))

OLD="$(current_version)"
OLD_CODE="$(current_code)"

if [ "$NEW" = "$OLD" ]; then
  echo "Already at version $OLD, nothing to do."
  exit 0
fi

echo "Bumping FoxMes version: $OLD ($OLD_CODE) -> $NEW ($NEW_CODE)"
echo

echo "Add a changelog.txt entry starting with \"$NEW \" first if you haven't - set_version.py requires it."
echo "Running Telegram/build/set_version.py (patches Telegram/build/version, core/version.h, Telegram.rc, Updater.rc, AppxManifest.xml)..."
python3 "$ROOT/Telegram/build/set_version.py" "$NEW"
echo

CHECK_VERSION="$(current_version)"
CHECK_CODE="$(current_code)"
if [ "$CHECK_VERSION" != "$NEW" ] || [ "$CHECK_CODE" != "$NEW_CODE" ]; then
  echo "ERROR: set_version.py produced $CHECK_VERSION ($CHECK_CODE), expected $NEW ($NEW_CODE)." >&2
  exit 1
fi

echo "Patching FoxMes-specific hardcoded version literals..."
build_patch_list "$OLD" "$OLD_CODE" "$NEW" "$NEW_CODE"
apply_patches
echo

echo "Verifying..."
build_patch_list "$NEW" "$NEW_CODE" "$NEW" "$NEW_CODE"
if check_patches; then
  echo
  echo "Version bumped: $OLD -> $NEW. Review the diff and commit."
else
  echo
  echo "ERROR: some files did not end up consistent - see above." >&2
  exit 1
fi
