#!/usr/bin/env bash
# The maintained full Doom port owns its build and launch configuration.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$ROOT/ports/doom/run-bf.sh" "$@"
