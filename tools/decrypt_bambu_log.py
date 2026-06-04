#!/usr/bin/env python3
"""
Decrypt a Bambu Studio plugin encrypted log file.

The proprietary `libbambu_networking.so` plugin writes its internal logs
to ~/.config/BambuStudio/log/debug_network_*.log.enc (and a parallel
studio_*_pid_enc.log.* set) using AES-128 in ECB mode with a static key
embedded in the plugin's rodata at VA 0x55985d:

    AES-128-ECB key = b"yyuBcftO2jkZeucy"   (16 ASCII bytes)

ECB is used unkeyed/IV-less, so each 16-byte ciphertext block decrypts
independently. The plaintext is spdlog-formatted UTF-8 text padded to a
16-byte boundary using NULs (\\0) rather than PKCS#7 (the trailing partial
block at EOF is just zero-padded).

Usage:
    decrypt_bambu_log.py <input.log.enc> [<output.log>]
    decrypt_bambu_log.py -    # read stdin, write to stdout
    decrypt_bambu_log.py --tail <file>    # like `tail -f` but decrypted

This file is keyed to the plugin version dumped at
/tmp/agent_a_work/dump/libbambu_networking_unpacked.so (BambuStudio
01.10.x family). If a future plugin rotates the key, re-run the rodata
scan in RE-LOG-ENCRYPTION.md.
"""
import os
import sys
import time
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PLUGIN_LOG_AES_KEY = b"yyuBcftO2jkZeucy"
BLOCK = 16


def decrypt_bytes(ct: bytes) -> bytes:
    """Decrypt an arbitrary length of plugin log ciphertext."""
    # Truncate any trailing partial block (the plugin may flush mid-block).
    aligned = ct[: len(ct) - (len(ct) % BLOCK)]
    if not aligned:
        return b""
    cipher = Cipher(algorithms.AES(PLUGIN_LOG_AES_KEY), modes.ECB())
    dec = cipher.decryptor()
    return dec.update(aligned) + dec.finalize()


def strip_log_padding(pt: bytes) -> bytes:
    """The plugin pads writes with NUL bytes to a 16-byte boundary.
    Collapse runs of NULs that appear at line boundaries; preserve text."""
    # Drop NUL bytes anywhere — they're never valid in spdlog text and only
    # appear as block padding.
    return pt.replace(b"\x00", b"")


def decrypt_file(in_path: str, out_path: str | None = None) -> bytes:
    with open(in_path, "rb") as f:
        ct = f.read()
    pt = strip_log_padding(decrypt_bytes(ct))
    if out_path:
        with open(out_path, "wb") as f:
            f.write(pt)
    return pt


def tail_decrypt(in_path: str) -> None:
    """Stream decrypted output as the file grows. Block-aligned."""
    offset = 0
    while True:
        size = os.path.getsize(in_path)
        if size > offset:
            aligned_size = size - (size % BLOCK)
            if aligned_size > offset:
                with open(in_path, "rb") as f:
                    f.seek(offset)
                    chunk = f.read(aligned_size - offset)
                pt = strip_log_padding(decrypt_bytes(chunk))
                try:
                    sys.stdout.buffer.write(pt)
                    sys.stdout.buffer.flush()
                except BrokenPipeError:
                    return
                offset = aligned_size
        time.sleep(0.5)


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__, file=sys.stderr)
        sys.exit(2)

    if argv[1] == "--tail":
        tail_decrypt(argv[2])
        return

    if argv[1] == "-":
        ct = sys.stdin.buffer.read()
        pt = strip_log_padding(decrypt_bytes(ct))
        sys.stdout.buffer.write(pt)
        return

    in_path = argv[1]
    out_path = argv[2] if len(argv) > 2 else None
    pt = decrypt_file(in_path, out_path)
    if not out_path:
        sys.stdout.buffer.write(pt)


if __name__ == "__main__":
    main(sys.argv)
