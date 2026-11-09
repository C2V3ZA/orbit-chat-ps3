#!/usr/bin/env bash
# Full app test with every connection going through a local Orbit relay (no direct path).
set -euo pipefail
cd "$(dirname "$0")/.."
rm -f build/relay-owners.json
python3 relay/orbit_relay.py --port 31400 --state build/relay-owners.json > build/relay.log 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true' EXIT
for _ in $(seq 50); do grep -q ready build/relay.log 2>/dev/null && break; sleep 0.1; done
ORBIT_TEST_RELAY=127.0.0.1:31400 timeout 300 ./build/test-app
python3 tests/relay_load.py 31400 | tail -1
echo "relay log:"
grep -E "online|offline" build/relay.log | head -6
