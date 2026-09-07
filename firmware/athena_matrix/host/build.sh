#!/usr/bin/env bash
# Build aura_host, the Mac-side driver for the real main/aura.c: the firmware
# renderer compiled with clang against stubs for the ESP-IDF headers and the
# hub75 driver (hub75_stub.c captures the frame instead of bit-banging GPIOs).
# Output: host/aura_host (gitignored). Takes well under a second.
set -e
cd "$(dirname "$0")"
MAIN=../main
HUB75=../components/hub75

cc -O2 -std=gnu11 -Wall -I stubs -I "$MAIN" -I "$HUB75/include" \
    "$MAIN/aura.c" "$HUB75/font5x7.c" \
    hub75_stub.c harness.c \
    -lm -o aura_host
echo "built $(pwd)/aura_host" >&2
