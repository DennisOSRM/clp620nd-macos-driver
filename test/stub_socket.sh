#!/bin/bash
# Stands in for the stock socket backend during tests: emits the STATE lines
# the real one would, drains stdin, and exits with $STUB_RC.
echo "DEBUG: stub: DEVICE_URI=$DEVICE_URI" >&2
echo "STATE: +media-jam-warning" >&2
echo "STATE: +toner-low-report" >&2
echo "STATE: +media-jam-warning,marker-supply-low-report" >&2
echo "PAGE: 1 1" >&2
cat > /dev/null
exit ${STUB_RC:-0}
