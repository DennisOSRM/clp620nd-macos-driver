#!/bin/bash
# Removes the queue and driver files.
# Usage: sudo ./scripts/uninstall.sh [queue-name]
set -euo pipefail
QUEUE="${1:-CLP620ND}"
[ "$(id -u)" -eq 0 ] || { echo "Must run as root:  sudo $0 $*" >&2; exit 1; }
lpadmin -x "$QUEUE" 2>/dev/null && echo "removed queue $QUEUE" || echo "no queue $QUEUE"
rm -rf /Library/Printers/Samsung/CLP-620ND
rm -f  /Library/Printers/PPDs/Contents/Resources/Samsung-CLP-620ND.ppd
rm -f  /usr/libexec/cups/backend/clp620
echo "driver files removed"
