#!/bin/bash
# Queries the printer over PJL (port 9100).
# Usage: ./scripts/probe.sh [ip] [query]   e.g. "INFO VARIABLES"
IP="${1:-192.168.179.180}"; Q="${2:-INFO CONFIG}"
printf '\033%%-12345X@PJL\r\n@PJL %s\r\n\033%%-12345X' "$Q" \
  | { cat; sleep 6; } | nc -w 9 "$IP" 9100 | LC_ALL=C tr -d '\r'
