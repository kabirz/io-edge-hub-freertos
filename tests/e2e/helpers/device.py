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
    """Send one request on sock and read exactly one response."""
    payload = body.encode() if isinstance(body, str) else body
    hdr = f"{method} {path} HTTP/1.1\r\nHost: {host}\r\n"
    if payload is not None:
        hdr += (f"Content-Type: application/json\r\n"
                f"Content-Length: {len(payload)}\r\n")
    sock.sendall((hdr + "\r\n").encode() + (payload or b""))
    return read_http_message(sock)


def _try_extract(buf):
    """尝试从 buf 解出一个完整响应 (Content-Length 或 chunked)。

    返回 (status, headers, body, leftover) 或 None (数据不完整)。
    """
    head, sep, rest = bytes(buf).partition(b"\r\n\r\n")
    if not sep:
        return None
    lines = head.decode("latin-1").split("\r\n")
    headers = {}
    clen = 0
    for line in lines[1:]:
        k, _, v = line.partition(":")
        key = k.strip().lower()
        headers[key] = v.strip()
        if key == "content-length":
            clen = int(v.strip())
    if headers.get("transfer-encoding", "").lower() == "chunked":
        body = bytearray()
        while True:
            nl = rest.find(b"\r\n")
            if nl < 0:
                return None
            line, rest = rest[:nl], rest[nl + 2:]
            size = int(line.split(b";")[0].strip() or b"0", 16)
            if size == 0:
                if rest.startswith(b"\r\n"):
                    rest = rest[2:]
                return lines[0], headers, bytes(body), rest
            if len(rest) < size + 2:
                return None
            body += rest[:size]
            rest = rest[size + 2:]
    if len(rest) < clen:
        return None
    return lines[0], headers, rest[:clen], rest[clen:]


def read_http_message(sock):
    msgs = read_http_messages(sock, 1)
    if not msgs:
        raise ConnectionError(f"no response; buffer incomplete")
    return msgs[0]


def read_http_messages(sock, count):
    """Read up to `count` pipelined responses from sock."""
    buf = bytearray()
    msgs = []
    while len(msgs) < count:
        while len(msgs) < count:
            r = _try_extract(bytes(buf))
            if r is None:
                break
            status, headers, body, rest = r
            msgs.append((status, headers, body))
            buf = bytearray(rest)
        if len(msgs) >= count:
            break
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
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
