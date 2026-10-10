#!/usr/bin/env bash
# Runs every automated check. Needs: PlatformIO, Node 18+, Python 3.
#   ./test/run_all.sh
set -euo pipefail
cd "$(dirname "$0")/.."

PIO="${PIO:-pio}"
command -v "$PIO" >/dev/null 2>&1 || PIO="$HOME/.platformio/penv/bin/pio"

# The firmware #includes secrets.h, which is gitignored - use the
# template for a build that only needs to compile.
CREATED_SECRETS=0
if [ ! -f include/secrets.h ]; then
  cp include/secrets.example.h include/secrets.h
  CREATED_SECRETS=1
fi
trap '[ "$CREATED_SECRETS" = 1 ] && rm -f include/secrets.h' EXIT

echo "== 1/4  Wiring: config.h vs the README table + diagrams, pin safety =="
python3 test/check_wiring.py
python3 tools/wiring_diagram.py --check

echo "== 2/4  Backend: Apps Script logic =="
node --test test/backend/appsscript.test.js

echo "== 3/4  Firmware logic: host unit tests =="
"$PIO" test -e native

echo "== 4/4  Firmware: compile for the ESP32 =="
"$PIO" run -e hardware

echo
echo "ALL CHECKS PASSED"
