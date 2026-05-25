"""SSDP normaliser.

Real Bambu printers emit two SSDP-shaped artefacts that the bridge mimics
byte-for-byte (see ~/BambuStudio/src/bambu_net_oss/core/SsdpListener.cpp
and src/bambu_bridge/server/SsdpResponder.cpp):

  - Unicast HTTP/1.1 200 OK reply to an M-SEARCH on udp/1900.
  - Multicast / broadcast NOTIFY (ssdp:alive | ssdp:byebye) to 1900 and
    255.255.255.255:2021 every 30 s.

Load-bearing variability that MUST be masked for a meaningful byte-diff:

  1. ``LOCATION:`` — the bridge emits a plain IP literal (NOT a
     UPnP-spec ``http://<ip>:<port>/upnp/desc.xml`` URL — see
     SsdpResponder.cpp's build_notify_headers comment for why). Real A1
     firmware does the same. Loopback tests use 127.0.0.x but a real
     printer announces its DHCP address, so we mask the value. We
     additionally accept the URL form so historical captures (or any
     future printer model that emits a UPnP-style URL) still normalise
     to a stable token.

  2. ``HOST: 239.255.255.250:1900`` (constant) — left alone.

  3. ``DevSignal.bambu.com:`` — the bridge's NOTIFY emits the bare
     integer ``-60`` per A1 firmware capture; M-SEARCH search responses
     omit the field entirely. Some captures (and older fixtures) carry
     the ``-50dBm`` shape. We mask either.

Things we deliberately leave unmasked because they are the diff's
load-bearing assertions:

  - USN: <dev_id>                   (the serial we're claiming to be)
  - DevName.bambu.com / DevModel.bambu.com / DevVersion.bambu.com
  - DevConnect.bambu.com: lan
  - DevBind.bambu.com / Devseclink.bambu.com / DevSecure.bambu.com
  - NT / NTS / ST
"""

import re


# Match LOCATION line. Two accepted shapes — UPnP-spec URL form and the
# bare-IP form the current bridge actually emits. Both normalise to the
# same `<NORMALISED-LAN-IP>` token so wire-diff captures from either kind
# of source line up.
_LOCATION_URL_RE = re.compile(
    rb"(?im)^(LOCATION:[ \t]*)http://[0-9.]+:[0-9]+/upnp/desc\.xml[ \t]*$",
)
_LOCATION_IP_RE = re.compile(
    rb"(?im)^(LOCATION:[ \t]*)[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+[ \t]*$",
)

# Match DevSignal line. Two shapes:  ``-50dBm`` (older / specced) and
# ``-60`` (current bridge / A1 capture).
_DEV_SIGNAL_DBM_RE = re.compile(
    rb"(?im)^(DevSignal\.bambu\.com:[ \t]*)-?\d+dBm[ \t]*$",
)
_DEV_SIGNAL_INT_RE = re.compile(
    rb"(?im)^(DevSignal\.bambu\.com:[ \t]*)-?\d+[ \t]*$",
)

# Match HOST line for NOTIFY frames — it's always the multicast group, but
# the trailing whitespace varies between hand-crafted and printer-emitted
# frames, so normalise the trailing CR(LF) to be lenient on whitespace.
# Header name itself is case-variable (``HOST:`` vs ``Host:``) — A1's
# capture uses uppercase; we accept either.
_HOST_RE = re.compile(
    rb"(?im)^(HOST:[ \t]*)239\.255\.255\.250:1900[ \t]*$",
)


def normalise(blob: bytes) -> bytes:
    """Return the SSDP blob with timestamp/IP/signal fields masked.

    Idempotent: ``normalise(normalise(x)) == normalise(x)``.
    """
    # Canonicalise line endings to LF so our per-line regexes don't
    # have to special-case CR.  We restore CRLF before returning.
    out = blob.replace(b"\r\n", b"\n")
    out = _LOCATION_URL_RE.sub(
        rb"\g<1><NORMALISED-LAN-IP>", out)
    out = _LOCATION_IP_RE.sub(
        rb"\g<1><NORMALISED-LAN-IP>", out)
    out = _DEV_SIGNAL_DBM_RE.sub(
        rb"\g<1><NORMALISED-SIGNAL>", out)
    out = _DEV_SIGNAL_INT_RE.sub(
        rb"\g<1><NORMALISED-SIGNAL>", out)
    out = _HOST_RE.sub(
        rb"\g<1>239.255.255.250:1900", out)
    out = out.replace(b"\n", b"\r\n")
    return out
