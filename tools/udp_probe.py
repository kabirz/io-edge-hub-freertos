"""UDP probe: GET_IP / GET_VERSION to device port 8600."""
import socket
import sys

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.12.101"

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2)
for name, payload in [("GET_IP(0x11)", b"\x11"), ("GET_VERSION(0x04)", b"\x04")]:
    s.sendto(payload, (IP, 8600))
    try:
        data, addr = s.recvfrom(256)
        print(name, "->", data.hex(), repr(data))
    except socket.timeout:
        print(name, "-> TIMEOUT")
s.close()
