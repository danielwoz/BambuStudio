#!/usr/bin/env bash
# Tail-decrypt the most recent BambuStudio proprietary plugin log.
#
# The libbambu_networking.so plugin writes AES-128-ECB encrypted spdlog
# output to ~/.config/BambuStudio/log/debug_network_*.log.enc.
# See tools/README.md and RE-LOG-ENCRYPTION.md for the cipher details.
#
# Usage:
#   tools/tail-plugin-log.sh            # tail the newest plugin log
#   tools/tail-plugin-log.sh <file>     # tail a specific .log.enc file
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECRYPT="${SCRIPT_DIR}/decrypt_bambu_log.py"

if [ $# -ge 1 ]; then
    TARGET="$1"
else
    TARGET=$(ls -t "${HOME}/.config/BambuStudio/log/"debug_network_*.log.enc 2>/dev/null | head -1 || true)
fi

if [ -z "${TARGET:-}" ] || [ ! -f "$TARGET" ]; then
    echo "No plugin log found under ~/.config/BambuStudio/log/debug_network_*.log.enc" >&2
    echo "Run the bridge or BambuStudio first to generate one." >&2
    exit 1
fi

echo "tailing $TARGET" >&2
exec python3 "$DECRYPT" --tail "$TARGET"
