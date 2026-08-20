"""Filter a COM9 capture: keep UDP/route/reboot/error lines, drop IP header art."""
import sys

d = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
keys = ("8600", "udp (", "route", "epoch", "build time", "icmp",
        "bad hdr", "reopen", "pbuf", "link", "MACRAW", "udpcfg")
lines = [l for l in d.splitlines() if any(k in l for k in keys)]
print(len(d), "bytes total,", len(lines), "matched lines")
print("\n".join(lines[:120]))
