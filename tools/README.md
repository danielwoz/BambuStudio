# Bridge dev tools

Small utilities that ride along with the BambuStudio-bridge source tree.
None of these are shipped with end-user binaries — they exist to make
debugging the bridge / plugin interaction less painful.

## decrypt_bambu_log.py

Decrypts the proprietary `libbambu_networking.so` plugin's encrypted
spdlog stream (`~/.config/BambuStudio/log/debug_network_*.log.enc`).

The plugin uses AES-128-ECB with the static 16-byte key
`yyuBcftO2jkZeucy` (extracted from rodata VA `0x55985d` of the
unpacked plugin). Full RE notes:
[`/mnt/cephfs/ssd/BambuBridge/RE-LOG-ENCRYPTION.md`](../../RE-LOG-ENCRYPTION.md)
(symlink target on this dev host).

### Modes

```bash
# One-shot: decrypt to stdout
python3 tools/decrypt_bambu_log.py path/to/debug_network_X.log.enc

# One-shot: decrypt to a file
python3 tools/decrypt_bambu_log.py in.log.enc out.log

# Stream from stdin
cat in.log.enc | python3 tools/decrypt_bambu_log.py -

# Live tail (like `tail -f`, block-aligned)
python3 tools/decrypt_bambu_log.py --tail in.log.enc
```

### Dependencies

```bash
pip3 install --user cryptography
```

The script uses `cryptography.hazmat.primitives.ciphers` for AES-ECB.

## tail-plugin-log.sh

Convenience wrapper. Finds the newest plugin log under
`~/.config/BambuStudio/log/` and runs `decrypt_bambu_log.py --tail`
on it.

```bash
# Tail the newest plugin log (auto-discovered)
tools/tail-plugin-log.sh

# Tail a specific file
tools/tail-plugin-log.sh ~/.config/BambuStudio/log/debug_network_Sat_May_23_02_17_54.log.enc
```

Pair it with a bridge run to get live observability of the plugin's
internal spdlog stream alongside the bridge's own logs.

### Typical debug session

```bash
# Terminal 1 — bridge
flock /tmp/bridge_runtime_lock \
    bambu-studio --bridge-only

# Terminal 2 — live plugin log
cd /path/to/BambuStudio-bridge
tools/tail-plugin-log.sh
```

## What the decrypted output looks like

Each line is spdlog-formatted:

```
[YYYY-MM-DD HH:MM:SS.ffffff +TZ] [L] [T <tid>]: <fmt-id><args...>
```

`<fmt-id>` is a small integer baked into the plugin at build time —
e.g. `340bambu_network_send_message-2` means *format-string-340*
(unknown literal, but consistently `"foo {} returned: {}"`-shaped),
with arg1=`bambu_network_send_message`, arg2=`-2`.

Even without the ID→literal-format-string table, you immediately get:

- precise timestamps with microseconds + timezone
- severity (`[I]`, `[W]`, `[E]`, `[D]`)
- thread IDs (correlate concurrent paths)
- interpolated arguments — URLs, dev_ids, error codes, hostnames

## Future work — format ID table

Building the ID → format-string map would let us substitute literal
fmt strings into the decrypted output. Pointers:

- Format strings live in `seg_va_005503c0.bin` rodata at offsets like
  `0x55d5` (`connect_server returned error:{}`), `0x5645`
  (`connect_printer returned error: {} {}`), and many adjacent.
- An ID→string mapping table was NOT obvious in scanned segments
  (no 8-byte LE pointer to the `connect_server` string appears in
  `seg_va_005503c0.bin`, `seg_va_006ba3c0.bin`, or
  `seg_va_0079a3c0.bin`).
- Likely route forward: trace each spdlog call site in Ghidra
  (xref-from a known string), capture the `mov r?d, <imm32>`
  immediately before, and emit `{imm32: literal_format}` pairs.
- Quick brute-force alternative: enumerate every `lea rXX, [rip + disp]`
  that targets a `...{}...` rodata string, find the nearest preceding
  immediate-mov, and pair them.

## Other files in this directory

- `no_debugger.c` — small LD_PRELOAD shim from earlier RE work.
- `vtun_test_client.py` — vtun-side test driver for the storage tunnel.
- `wire_diff/` — JSON-diff utilities for MQTT capture comparison.
