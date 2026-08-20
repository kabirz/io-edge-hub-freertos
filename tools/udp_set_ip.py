"""SET_IP + REBOOT to move device off a conflicting address."""
import socket
import sys
import time

cur = sys.argv[1] if len(sys.argv) > 1 else "192.168.12.101"
new = sys.argv[2] if len(sys.argv) > 2 else "192.168.12.177"
newb = bytes(int(x) for x in new.split("."))

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2)

# SET_IP [0x10][a][b][c][d] -> [0x10][ok]
s.sendto(b"\x10" + newb, (cur, 8600))
try:
    data, _ = s.recvfrom(256)
    print("SET_IP ->", data.hex())
except socket.timeout:
    print("SET_IP -> TIMEOUT")

time.sleep(0.3)

# REBOOT [0x05] -> [0x05][ok] then cold reset
s.sendto(b"\x05", (cur, 8600))
try:
    data, _ = s.recvfrom(256)
    print("REBOOT ->", data.hex())
except socket.timeout:
    print("REBOOT -> TIMEOUT")
s.close()

print("waiting for device at", new, "...")
for i in range(15):
    time.sleep(1)
    r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    r.settimeout(1)
    r.sendto(b"\x11", (new, 8600))
    try:
        data, _ = r.recvfrom(256)
        print("GET_IP @", new, "->", data.hex())
        r.close()
        break
    except socket.timeout:
        pass
    r.close()
else:
    print("device did not come back at", new)
