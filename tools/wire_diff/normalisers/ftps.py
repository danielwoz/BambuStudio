"""FTPS normaliser.

FTPS exchanges (the bridge's FtpsServer + a libcurl-shaped client) are
line-based ASCII inside a TLS tunnel.  Captures are taken post-TLS so
they're plain CRLF text.

Variable bytes that must be masked:

  1. The PASV reply's host quartet *and* port pair: real Bambu printers
     advertise their LAN IP (192.168.x.y); the bridge's loopback test
     advertises 127.0.0.x; the port is kernel-picked from the ephemeral
     range each session.

       227 Entering Passive Mode (192,168,1,209,234,17).
                                  ^^^^^^^^^^^^^^^^^^^^^
                                  IP quartet + port pair

  2. The EPSV reply's port:

       229 Entering Extended Passive Mode (|||43221|)
                                              ^^^^^

  3. Timestamps in LIST/NLST output (the bridge always returns an empty
     body so this is a no-op for our fixture; included so future real-
     printer captures with non-empty LIST output normalise correctly).

  4. The welcome banner's product string (real printers emit
     "(vsFTPd 3.0.3)"; the bridge emits "Bambu Bridge FTPS ready.")  — we
     mask the parenthetical so the only thing left is the 220 code.  For
     phase 11 this is a known difference flagged for phase 12.

Things we deliberately keep:

  - The numeric reply codes (220, 331, 230, 200, 257, 250, 226, 221, …)
  - The command verbs (USER, PASS, PBSZ, PROT, TYPE, PWD, CWD, MKD, PASV,
    EPSV, STOR, LIST, NLST, QUIT, NOOP, FEAT, OPTS, SYST, REST, SIZE,
    MDTM, DELE, RMD, CDUP, XPWD, XCWD, XMKD)
  - The path argument to STOR/CWD/MKD
"""

import re


# PASV reply:  227 Entering Passive Mode (a,b,c,d,p1,p2).
_PASV_RE = re.compile(
    rb"(?im)^(227\s+)"
    rb".*\(\d{1,3},\d{1,3},\d{1,3},\d{1,3},\d{1,3},\d{1,3}\).*$",
)

# EPSV reply:  229 Entering Extended Passive Mode (|||<port>|)
_EPSV_RE = re.compile(
    rb"(?im)^(229\s+)"
    rb".*\(\|\|\|\d+\|\).*$",
)

# LIST entry timestamps: e.g.
#   -rw-r--r-- 1 ftp ftp 12345 Jan 12 13:45 file.3mf
# Mask the "Mon DD HH:MM" / "Mon DD  YYYY" date span.
_LIST_DATE_RE = re.compile(
    rb"(?m)("
    rb"[-rwx]{10}\s+\d+\s+\S+\s+\S+\s+\d+\s+)"
    rb"(?:[A-Z][a-z]{2}\s+\d{1,2}\s+(?:\d{2}:\d{2}|\d{4}))"
    rb"(\s+\S+)",
)

# Generic FTP reply line:  "<code><sp>|<code>-<prose>" — three-digit
# reply code followed by either a space or a dash, then a free-form
# message.  Real BambuStudio (via libcurl) only inspects the numeric
# code; vsFTPd, the bridge, and proftpd all emit slightly different
# prose, so we mask everything after the code+separator.  This makes
# the diff purely structural (which codes appeared, in what order).
_REPLY_LINE_RE = re.compile(
    rb"(?m)^(\d{3})([ -]).*$",
)


def normalise(blob: bytes) -> bytes:
    # Canonicalise to LF for the regex pass; we restore CRLF before
    # returning.  This keeps line-anchored regexes from accidentally
    # consuming \r across line boundaries.
    out = blob.replace(b"\r\n", b"\n")
    # Order matters — PASV/EPSV first so the PASV-tuple placeholder is
    # in place before the generic reply-line mask runs and would
    # otherwise swallow it.
    out = _PASV_RE.sub(
        rb"\g<1>(<NORMALISED-PASV-IP-PORT>)", out)
    out = _EPSV_RE.sub(
        rb"\g<1>(|||<NORMALISED-EPSV-PORT>|)", out)
    out = _LIST_DATE_RE.sub(
        rb"\g<1><NORMALISED-LIST-DATE>\g<2>", out)

    def _mask_reply(m: "re.Match[bytes]") -> bytes:
        code, sep = m.group(1), m.group(2)
        whole = m.group(0)
        if b"<NORMALISED-PASV-IP-PORT>" in whole \
                or b"<NORMALISED-EPSV-PORT>" in whole:
            return whole
        return code + sep + b"<NORMALISED-REPLY-PROSE>"
    out = _REPLY_LINE_RE.sub(_mask_reply, out)
    out = out.replace(b"\n", b"\r\n")
    return out
