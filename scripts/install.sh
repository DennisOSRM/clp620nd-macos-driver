#!/bin/bash
# Installs the driver and creates a CUPS queue.
# Usage: sudo ./scripts/install.sh [printer-ip] [queue-name]
set -euo pipefail

IP="${1:-192.168.179.180}"
QUEUE="${2:-CLP620ND}"
# $3 overrides the device URI. The default goes through the clp620 backend,
# which is socket:// plus a filter for the firmware's bogus paper-jam report
# (see README). Pass socket://... to bypass it, or a usb://... URI
# (see `lpinfo -v`) to drive the printer over USB instead.
DEVICE="${3:-clp620://$IP:9100}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DEST="/Library/Printers/Samsung/CLP-620ND/filter"
PPDDIR="/Library/Printers/PPDs/Contents/Resources"
BACKENDDIR="/usr/libexec/cups/backend"

[ "$(id -u)" -eq 0 ] || { echo "Must run as root:  sudo $0 $*" >&2; exit 1; }

echo "==> filter -> $DEST"
install -d -m 755 "$DEST"
[ -x "$ROOT/filter/rastertoclp620" ] || { echo "Build it first: make" >&2; exit 1; }
install -m 755 "$ROOT/filter/rastertoclp620" "$DEST/rastertoclp620"

# The backend only matters for the default clp620:// URI. If the directory is
# not writable (a future macOS locking it down), fall back to plain socket://
# rather than creating a queue that cannot print.
if [ "${DEVICE#clp620://}" != "$DEVICE" ]; then
  echo "==> backend -> $BACKENDDIR/clp620"
  [ -x "$ROOT/backend/clp620" ] || { echo "Build it first: make" >&2; exit 1; }
  if ! install -m 555 -o root -g wheel "$ROOT/backend/clp620" "$BACKENDDIR/clp620" 2>/dev/null; then
    echo "    WARNING: cannot install into $BACKENDDIR; using socket:// instead." >&2
    echo "    The spurious paper-jam report will not be filtered." >&2
    DEVICE="socket://$IP:9100"
  fi
fi

echo "==> PPD -> $PPDDIR"
install -d -m 755 "$PPDDIR"
install -m 644 "$ROOT/ppd/Samsung-CLP-620ND.ppd" "$PPDDIR/Samsung-CLP-620ND.ppd"

echo "==> queue '$QUEUE' -> $DEVICE"
lpadmin -p "$QUEUE" \
        -v "$DEVICE" \
        -P "$PPDDIR/Samsung-CLP-620ND.ppd" \
        -D "Samsung CLP-620ND (Colour)" \
        -L "$IP" \
        -o printer-is-shared=false \
        -E

# Colour-first defaults.  SNMP is left on so toner levels keep being reported;
# the clp620 backend filters the firmware's bogus media-jam-warning instead of
# switching SNMP off wholesale.  See README.
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
