#!/usr/bin/env bash
set -euo pipefail

# Runs FoxMes against the production API baked into the binary.
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "${DIR}/dev-client.sh" prod "$@"
