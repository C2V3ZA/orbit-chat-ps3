#!/usr/bin/env bash
# Full app test through the relay's built-in bore client and a local bore stand-in:
# friend -> "public" tunnel port -> bore protocol -> relay -> host (found in the home network).
set -euo pipefail
cd "$(dirname "$0")/.."
rm -f build/relay-owners.json
python3 tests/fake_bore.py 37835 31477 > build/fake-bore.log 2>&1 &
bpid=$!
python3 relay/orbit_relay.py --port 31400 --state build/relay-owners.json \
    --bore 127.0.0.1:37835 --bore-port 31477 > build/relay-bore.log 2>&1 &
rpid=$!
trap 'kill $bpid $rpid 2>/dev/null || true' EXIT
for _ in $(seq 50); do grep -q "Internet-Adresse" build/relay-bore.log 2>/dev/null && break; sleep 0.1; done
grep "Internet-Adresse" build/relay-bore.log | head -1
ORBIT_TEST_RELAY=127.0.0.1:31477 timeout 300 ./build/test-app
python3 tests/relay_load.py 31477 31400 | tail -1
