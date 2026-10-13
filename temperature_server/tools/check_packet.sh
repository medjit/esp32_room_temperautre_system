#!/usr/bin/env bash
# check_packet.sh - proves that the server's node_packet.h still matches the node firmware.
#
# Implements REQ-GEN-06: the block between "BEGIN NodePacket" and "END NodePacket" must be
# byte-for-byte identical in both projects. Exit code 0 = identical, 1 = different.
#
# Usage: tools/check_packet.sh

set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
node="$here/../temperature_ndoe/temperature_node_src/temperature_node_src.ino"
server="$here/temperature_server_src/node_packet.h"

# Print only the lines from the BEGIN marker to the END marker.
block() { sed -n '/===== BEGIN NodePacket/,/===== END NodePacket/p' "$1"; }

if diff <(block "$node") <(block "$server") > /dev/null; then
    echo "OK: NodePacket block is identical in node and server."
else
    echo "MISMATCH: NodePacket differs between node and server:" >&2
    diff <(block "$node") <(block "$server") >&2 || true
    exit 1
fi
