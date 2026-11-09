#!/usr/bin/env bash
# Full app test through the PS3's built-in tunnel (bore protocol) and a local bore stand-in.
# No PC relay involved: friend -> tunnel port -> bore -> host console.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tests/fake_bore.py 37836 31478 > build/fake-bore-ps3.log 2>&1 &
bpid=$!
trap 'kill $bpid 2>/dev/null || true' EXIT
for _ in $(seq 50); do grep -q ready build/fake-bore-ps3.log 2>/dev/null && break; sleep 0.1; done
ORBIT_TEST_TUNNEL=127.0.0.1:37836 timeout 300 ./build/test-app
grep "tunnel open" build/fake-bore-ps3.log | head -3
