#!/usr/bin/env bash
# Runs the router-opening code against a simulated router (no real router is touched).
set -euo pipefail
cd "$(dirname "$0")/.."
MBED=../build-tools/mbedtls-2.28.10
cc -std=gnu11 -O2 -Wall -Wextra -Iinclude -Ivendor -I$MBED/include src/nat.c src/platform.c \
    vendor/monocypher.c vendor/cJSON.c tests/test_nat.c -o build/test-nat
for mode in upnp pcp cgnat fritz; do
    python3 tests/fake_router.py "$mode" "build/fake-router-$mode.log" > "build/fake-router-$mode.out" &
    pid=$!
    for _ in $(seq 50); do grep -q ready "build/fake-router-$mode.out" 2>/dev/null && break; sleep 0.1; done
    if ! ./build/test-nat "$mode"; then kill $pid; exit 1; fi
    kill $pid
    wait $pid 2>/dev/null || true
    sleep 1 # let the previous simulated router release its ports
done
