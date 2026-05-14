"""SSDP normaliser.

Real Bambu printers emit two SSDP-shaped artefacts that the bridge mimics
byte-for-byte (see ~/BambuStudio/src/bambu_net_oss/core/SsdpListener.cpp
and src/bambu_bridge/server/SsdpResponder.cpp):

  - Unicast HTTP/1.1 200 OK reply to an M-SEARCH on udp/1900.
  - Multicast / broadcast NOTIFY (ssdp:alive | ssdp:byebye) to 1900 and
    255.255.255.255:2021 every 30 s.

Load-bearing variability that MUST be masked for a meaningful byte-diff:

  1. ``LOCATION: http://<lan_ip>:<http_port>/upnp/desc.xml``
     The LAN IP depends on which interface the host announces on; for the
     loopback test it's 127.0.0.x but a real printer announces its DHCP
     address.  We mask the host:port pair.

  2. ``HOST: 239.255.255.250:1900`` (constant) — left alone.

  3. ``DevSignal.bambu.com: -50dBm`` — the bridge emits a constant value
     but a real printer's signal strength varies session-to-session, so we
     mask the dB value.

Things we deliberately leave unmasked because they are the diff's
load-bearing assertions:

  - USN: <dev_id>                   (the serial we're claiming to be)
  - DevName.bambu.com / DevModel.bambu.com / DevVersion.bambu.com
  - DevConnect.bambu.com: lan
  - DevBind.bambu.com / Devseclink.bambu.com / DevSecure.bambu.com
  - NT / NTS / ST
"""

import re


# Match LOCATION line:  LOCATION: http://1.2.3.4:80/upnp/desc.xml
# (case-insensitive header name; HTTP shapes are CRLF-terminated).
_LOCATION_RE = re.compile(
    rb"(?im)^(LOCATION:[ \t]*http://)[0-9.]+:[0-9]+(/upnp/desc\.xml)[ \t]*$",
)

# Match DevSignal line:  DevSignal.bambu.com: -50dBm
_DEV_SIGNAL_RE = re.compile(
    rb"(?im)^(DevSignal\.bambu\.com:[ \t]*)-?\d+dBm[ \t]*$",
)

# Match HOST line for NOTIFY frames — it's always the multicast group, but
# the trailing whitespace varies between hand-crafted and printer-emitted
# frames, so normalise the trailing CR(LF) to be lenient on whitespace.
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
    out = _LOCATION_RE.sub(
        rb"\g<1><NORMALISED-LAN-IP>:<NORMALISED-PORT>\g<2>", out)
    out = _DEV_SIGNAL_RE.sub(
        rb"\g<1><NORMALISED-SIGNAL>", out)
    out = _HOST_RE.sub(
        rb"\g<1>239.255.255.250:1900", out)
    out = out.replace(b"\n", b"\r\n")
    return out
