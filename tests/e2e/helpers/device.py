"""Device access helpers: bound TCP/UDP/HTTP/FTP clients.

All client sockets bind to src_ip when set — the bench PC runs a TUN proxy
that can otherwise intercept traffic to the device subnet.
"""
import ftplib
import json
import re
import socket
import subprocess

UDP_CFG_PORT = 8600
HTTP_PORT = 80
FTP_PORT = 21
MODBUS_PORT = 502


class Device:
    def __init__(self, ip, src_ip=None, timeout=5.0):
        self.ip = ip
        self.src = src_ip
        self.timeout = timeout

    def tcp(self, port, timeout=None):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if self.src:
            s.bind((self.src, 0))
        s.settimeout(timeout or self.timeout)
        s.connect((self.ip, port))
        return s

    def udp(self, timeout=None):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if self.src:
            s.bind((self.src, 0))
        s.settimeout(timeout or self.timeout)
        return s

    def udp_xfer(self, payload, timeout=None, port=UDP_CFG_PORT):
        s = self.udp(timeout)
        try:
            s.sendto(payload, (self.ip, port))
            r, _ = s.recvfrom(512)
            return r
        finally:
            s.close()

    # ---- HTTP ----

    def http(self, method, path, body=None, timeout=None):
        """One request on a fresh connection. Returns (status, headers, body)."""
        s = self.tcp(HTTP_PORT, timeout)
        try:
            return http_on(s, method, path, body, host=self.ip)
        finally:
            s.close()

    def http_json(self, method, path, body=None, timeout=None):
        st, _, body = self.http(method, path, body, timeout)
        return st, json.loads(body)

    # ---- FTP ----

    def ftp(self, user="admin", password="admin", timeout=None):
        f = ftplib.FTP()
        source_address = (self.src, 0) if self.src else None
        f.connect(self.ip, FTP_PORT, timeout or self.timeout,
                  source_address=source_address)
        f.login(user, password)
        return f


def http_on(sock, method, path, body=None, host="device"):
    """Send one request on sock and read exactly one Content-Length response."""
    payload = body.encode() if isinstance(body, str) else body
    hdr = f"{method} {path} HTTP/1.1\r\nHost: {host}\r\n"
    if payload is not None:
        hdr += (f"Content-Type: application/json\r\n"
                f"Content-Length: {len(payload)}\r\n")
    sock.sendall((hdr + "\r\n").encode() + (payload or b""))
    return read_http_message(sock)


def read_http_message(sock):
    buf = bytearray()
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError(f"connection closed: {bytes(buf)!r}")
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    clen = 0
    headers = {}
    lines = head.decode("latin-1").split("\r\n")
    for line in lines[1:]:
        k, _, v = line.partition(":")
        headers[k.strip().lower()] = v.strip()
        if k.strip().lower() == "content-length":
            clen = int(v.strip())
    while len(rest) < clen:
        chunk = sock.recv(4096)
        if not chunk:
            break
        rest += chunk
    return lines[0], headers, bytes(rest[:clen])


def read_http_messages(sock, count):
    """Read `count` pipelined responses from sock."""
    buf = bytearray()
    msgs = []
    while len(msgs) < count:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
        while len(msgs) < count:
            head, sep, rest = bytes(buf).partition(b"\r\n\r\n")
            if not sep:
                break
            clen = 0
            for line in head.split(b"\r\n")[1:]:
                if line.lower().startswith(b"content-length:"):
                    clen = int(line.split(b":")[1])
            if len(rest) < clen:
                break
            msgs.append((head.decode("latin-1").split("\r\n")[0], rest[:clen]))
            buf = rest[clen:]
    return msgs


def parse_version(udp_reply):
    """UDP GET_VERSION reply -> 'vM.m.p_git'."""
    m = re.match(rb"^\x04v(\d+)\.(\d+)\.(\d+)_(.{0,6})", udp_reply, re.S)
    assert m, udp_reply
    return m.group(0)[1:].decode("latin-1").rstrip("\0")


def autodetect_src(ip):
    """Pick a local IPv4 sharing the device's /24 (bypasses the TUN proxy)."""
    out = subprocess.run(["ipconfig"], capture_output=True,
                         text=True, errors="replace").stdout
    net = ip.rsplit(".", 1)[0]
    for addr in re.findall(r"\b(?:\d{1,3}\.){3}\d{1,3}\b", out):
        if addr.startswith(net + "."):
            return addr
    return None
