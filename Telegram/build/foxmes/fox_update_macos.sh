#!/bin/bash
# Swaps a running FoxMes.app for the build inside a verified .dmg.
#
# Started detached by the app right before it quits: a bundle cannot rewrite
# itself while its own binary is mapped, so this waits for the process to go
# away first. The client has already checked the dmg's SHA-256 against the
# release manifest - nothing here re-downloads or re-verifies, and nothing
# here runs unless that check passed.
#
# Arguments: <dmg> <bundle> <pid>
set -u

dmg="${1:-}"
bundle="${2:-}"
pid="${3:-}"

log_dir="$HOME/Library/Logs/FoxMes"
log_file="$log_dir/update.log"
mkdir -p "$log_dir"

log() {
	printf '%s %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$*" >> "$log_file"
}

fail() {
	log "ERROR: $*"
	exit 1
}

if [ -z "$dmg" ] || [ -z "$bundle" ] || [ -z "$pid" ]; then
	fail "usage: fox_update_macos.sh <dmg> <bundle> <pid>"
fi

log "update requested: dmg=$dmg bundle=$bundle pid=$pid"

test -f "$dmg" || fail "no such dmg: $dmg"
test -d "$bundle" || fail "no such bundle: $bundle"

# The app quits on its own; this only refuses to touch the bundle while it is
# still alive. No force kill: losing unsaved state to save a few seconds is a
# bad trade, and the next hourly check would offer the update again anyway.
waited=0
while kill -0 "$pid" 2>/dev/null; do
	if [ "$waited" -ge 150 ]; then
		fail "process $pid is still running after 30s"
	fi
	sleep 0.2
	waited=$((waited + 1))
done
log "process $pid exited after $((waited / 5))s"

mount_point="$(mktemp -d /tmp/foxmes-update.XXXXXX)" || fail "mktemp failed"

cleanup() {
	if mount | grep -q " $mount_point "; then
		hdiutil detach "$mount_point" -quiet || hdiutil detach "$mount_point" -force -quiet || true
	fi
	rmdir "$mount_point" 2>/dev/null || true
}
trap cleanup EXIT

hdiutil attach "$dmg" \
	-mountpoint "$mount_point" \
	-nobrowse \
	-noautoopen \
	-quiet || fail "hdiutil attach failed"
log "mounted at $mount_point"

source_app="$mount_point/FoxMes.app"
test -d "$source_app" || fail "no FoxMes.app inside the dmg"

# ditto, not cp -R: it preserves the bundle's metadata and extended
# attributes, which is what keeps the code signature intact.
ditto "$source_app" "$bundle" || fail "ditto failed"
log "copied into $bundle"

hdiutil detach "$mount_point" -quiet || hdiutil detach "$mount_point" -force -quiet || true
rmdir "$mount_point" 2>/dev/null || true
trap - EXIT

# ditto carries com.apple.quarantine across with everything else. Removing it
# does not touch the signature, and on an ad-hoc signed build without a
# notarization ticket it is what lets the fresh copy open without Gatekeeper
# stopping it. Idempotent: harmless when the attribute was never set.
xattr -dr com.apple.quarantine "$bundle" 2>/dev/null || true
log "quarantine cleared"

rm -f "$dmg"

open "$bundle" || fail "cannot launch $bundle"
log "relaunched $bundle"
