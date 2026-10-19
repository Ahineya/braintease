#!/usr/bin/env bash
# Run Riplix using the shared launcher; preserve existing build and disk paths.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "$ROOT/scripts/run-fuzix-bf.sh" "$@"
