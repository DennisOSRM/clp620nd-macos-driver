#!/bin/bash
# Reports printer state and supply levels over SNMP, and decodes
# hrPrinterDetectedErrorState bit by bit.
# Usage: ./scripts/check-state.sh [ip]
IP="${1:-192.168.179.180}"
C=public

hex=$(snmpget -v1 -c $C -t 3 -r 2 -Oqv $IP 1.3.6.1.2.1.25.3.5.1.2.1 2>/dev/null | tr -dc '0-9A-Fa-f')
hex=${hex:-0000}
b1=$((16#${hex:0:2})); b2=$((16#${hex:2:2}))
echo "hrPrinterDetectedErrorState = ${hex:-??}"
for e in "128 lowPaper" "64 noPaper" "32 lowToner" "16 noToner" \
         "8 doorOpen" "4 jammed" "2 offline" "1 serviceRequested"; do
    set -- $e; [ $((b1 & $1)) -ne 0 ] && echo "    SET: $2"
done
for e in "128 inputTrayMissing" "64 outputTrayMissing" "32 markerSupplyMissing" \
         "16 outputNearFull" "8 outputFull" "4 inputTrayEmpty" "2 overduePreventMaint"; do
    set -- $e; [ $((b2 & $1)) -ne 0 ] && echo "    SET: $2"
done
[ "$hex" = "0000" ] && echo "    (no error bits)"

echo
echo "panel:  $(snmpget -v1 -c $C -t 3 -Oqv $IP 1.3.6.1.2.1.43.16.5.1.2.1.1 2>/dev/null)"
echo "status: $(snmpget -v1 -c $C -t 3 -Oqv $IP 1.3.6.1.2.1.25.3.5.1.1.1 2>/dev/null)"

echo
echo "trays:"
paste <(snmpwalk -v1 -c $C -t 3 -Oqv $IP 1.3.6.1.2.1.43.8.2.1.13 2>/dev/null) \
      <(snmpwalk -v1 -c $C -t 3 -Oqv $IP 1.3.6.1.2.1.43.8.2.1.10 2>/dev/null) \
      <(snmpwalk -v1 -c $C -t 3 -Oqv $IP 1.3.6.1.2.1.43.8.2.1.11 2>/dev/null) \
  | awk -F'\t' '{printf "  %-12s level=%-6s status=%s\n",$1,$2,$3}'

echo
echo "supplies:"
paste <(snmpwalk -v1 -c $C -t 3 -Oqv $IP 1.3.6.1.2.1.43.11.1.1.6 2>/dev/null | tr -d '"') \
      <(snmpwalk -v1 -c $C -t 3 -Oqv $IP 1.3.6.1.2.1.43.11.1.1.9 2>/dev/null) \
      <(snmpwalk -v1 -c $C -t 3 -Oqv $IP 1.3.6.1.2.1.43.11.1.1.8 2>/dev/null) \
  | awk -F'\t' '{ if ($3>0) pct=int($2*100/$3); else pct=-1;
                  printf "  %-46s %s/%s (%s%%)\n", substr($1,1,46), $2, $3, pct }'

echo
echo "pending alerts:"
snmpwalk -v1 -c $C -t 3 -Oqv $IP 1.3.6.1.2.1.43.18.1.1.8 2>/dev/null \
  | sed 's/^/  /' | cut -c1-100
