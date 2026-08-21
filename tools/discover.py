"""Broadcast GET_IP discovery: find the device wherever it lives now."""
import socket
import struct

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.settimeout(2)
s.bind(("192.168.12.150", 0))
s.sendto(b"\x11", ("255.255.255.255", 8600))
try:
    for _ in range(5):
        data, addr = s.recvfrom(256)
        ip = ".".join(str(b) for b in data[1:5])
        print("device at", addr[0], "reports IP", ip)
except socket.timeout:
    print("no reply to broadcast GET_IP")
s.close()
