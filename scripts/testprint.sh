#!/bin/bash
# Prints the colour test page through a CUPS queue.
# Usage: ./scripts/testprint.sh [queue] [extra lp options...]
set -euo pipefail
QUEUE="${1:-CLP620ND}"; shift || true
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
lp -d "$QUEUE" "$@" "$ROOT/test/testpage.pdf"
