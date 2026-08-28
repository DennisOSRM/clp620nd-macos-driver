#!/bin/bash
# Installs the driver and creates a CUPS queue.
# Usage: sudo ./scripts/install.sh [printer-ip] [queue-name]
set -euo pipefail

IP="${1:-192.168.179.180}"
QUEUE="${2:-CLP620ND}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DEST="/Library/Printers/Samsung/CLP-620ND/filter"
PPDDIR="/Library/Printers/PPDs/Contents/Resources"

[ "$(id -u)" -eq 0 ] || { echo "Must run as root:  sudo $0 $*" >&2; exit 1; }

echo "==> filter -> $DEST"
install -d -m 755 "$DEST"
[ -x "$ROOT/filter/rastertoclp620" ] || { echo "Build it first: make" >&2; exit 1; }
install -m 755 "$ROOT/filter/rastertoclp620" "$DEST/rastertoclp620"

echo "==> PPD -> $PPDDIR"
install -d -m 755 "$PPDDIR"
install -m 644 "$ROOT/ppd/Samsung-CLP-620ND.ppd" "$PPDDIR/Samsung-CLP-620ND.ppd"

echo "==> queue '$QUEUE' -> socket://$IP:9100"
lpadmin -p "$QUEUE" \
        -v "socket://$IP:9100" \
        -P "$PPDDIR/Samsung-CLP-620ND.ppd" \
        -D "Samsung CLP-620ND (Colour)" \
        -L "$IP" \
        -o printer-is-shared=false \
        -E

# Colour-first defaults.  SNMP is left on so toner levels are reported; see
# README for the MP-tray cause of any spurious media-jam-warning.
lpadmin -p "$QUEUE" \
        -o ColorModel-default=RGB \
        -o Resolution-default=600dpi \
        -o PageSize-default=A4 \
        -o MediaType-default=PLAIN \
        -o InputSlot-default=Auto \
        -o Duplex-default=None

cupsenable "$QUEUE"; cupsaccept "$QUEUE"
echo "==> done. Test with:  ./scripts/testprint.sh $QUEUE"
lpstat -p "$QUEUE" -l | head -6
