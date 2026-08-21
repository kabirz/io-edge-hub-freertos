"""Web server probe: page + all REST endpoints (bind physical NIC to
bypass the local TUN proxy)."""
import gzip
import json
import socket
import sys
import time

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.12.101"
SRC = sys.argv[2] if len(sys.argv) > 2 else None
PORT = 80


def connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(4)
    if SRC:
        s.bind((SRC, 0))
    s.connect((IP, PORT))
    return s


def recv_response(s):
    """Read one HTTP response (simple: read headers, then Content-Length body)."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            return None, None, buf
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    clen = 0
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":")[1])
    body = rest
    while len(body) < clen:
        chunk = s.recv(4096)
        if not chunk:
            break
        body += chunk
    status = head.split(b"\r\n")[0].decode()
    return status, head.decode(), body


def req(s, method, path, body=None):
    b = body.encode() if body else None
    hdr = f"{method} {path} HTTP/1.1\r\nHost: {IP}\r\n"
    if b:
        hdr += f"Content-Type: application/json\r\nContent-Length: {len(b)}\r\n"
    hdr += "\r\n"
    s.sendall(hdr.encode() + (b or b""))
    return recv_response(s)


def main():
    ok = True

    # 1. page (gzip)
    s = connect()
    status, head, body = req(s, "GET", "/")
    page = gzip.decompress(body)
    assert status.startswith("HTTP/1.1 200"), status
    assert b"<html" in page.lower() and len(page) > 20000, len(page)
    print(f"GET / -> 200, {len(body)}B gz -> {len(page)}B html")
    # keep-alive: second request on same conn
    status, _, body = req(s, "GET", "/api/io")
    assert status.startswith("HTTP/1.1 200"), status
    io = json.loads(body)
    assert io["t"] == "io" and len(io["di"]) == 16 and len(io["do"]) == 8 and len(io["ai"]) == 4
    print(f"GET /api/io (keep-alive) -> di={io['di'][:4]}.. do={io['do']} ai={io['ai']}")
    s.close()

    # 2. info / regs
    s = connect()
    status, _, body = req(s, "GET", "/api/info")
    info = json.loads(body)
    assert info["t"] == "info" and info["board"] == "io_edge_f407vet6"
    print(f"GET /api/info -> v{info['version'] if 'version' not in info else info['version']}"
          f" ip={info['ip']} mac={info['mac']} lfs={info['lfs_free']}/{info['lfs_total']}")
    status, _, body = req(s, "GET", "/api/regs")
    regs = json.loads(body)
    assert len(regs["holding"]) == 18 and len(regs["input"]) == 6
    print(f"GET /api/regs -> holding={regs['holding'][:6]}.. input={regs['input']}")
    s.close()

    # 3. DO control roundtrip
    s = connect()
    status, _, body = req(s, "POST", "/api/do", '{"index":0,"value":1}')
    assert json.loads(body)["ok"] is True
    _, _, body = req(s, "GET", "/api/io")
    assert json.loads(body)["do"][0] == 1
    status, _, body = req(s, "POST", "/api/do", '{"index":0,"value":0}')
    assert json.loads(body)["ok"] is True
    _, _, body = req(s, "GET", "/api/io")
    assert json.loads(body)["do"][0] == 0
    print("POST /api/do 0->1->0 roundtrip ok")
    s.close()

    # 4. invalid requests
    s = connect()
    status, _, body = req(s, "POST", "/api/do", '{"index":9,"value":1}')
    assert "400" in status, status
    status, _, body = req(s, "GET", "/api/nonexistent")
    assert "404" in status, status
    print("400/404 error paths ok")
    s.close()

    # 5. time + save + cfg
    s = connect()
    status, _, body = req(s, "POST", "/api/time", json.dumps({"ts": int(time.time())}))
    assert json.loads(body)["ok"] is True
    status, _, body = req(s, "POST", "/api/cfg", '{"sid":2}')
    assert json.loads(body)["ok"] is True
    _, _, body = req(s, "GET", "/api/info")
    assert json.loads(body)["slave_id"] == 2
    status, _, body = req(s, "POST", "/api/cfg", '{"sid":1}')
    assert json.loads(body)["ok"] is True
    status, _, body = req(s, "POST", "/api/cfg", '{"sid":999}')
    assert "400" in status and "slave" in json.loads(body)["err"]
    status, _, body = req(s, "POST", "/api/save", "{}")
    assert json.loads(body)["ok"] is True
    print("time/cfg(sid 2->1, invalid rejected)/save ok")
    s.close()

    # 6. history list + bad download
    s = connect()
    status, _, body = req(s, "GET", "/api/history")
    files = json.loads(body)["files"]
    print(f"GET /api/history -> {len(files)} files: {[f['name'] for f in files][:3]}")
    status, _, body = req(s, "GET", "/api/history/download?name=../etc/passwd")
    assert "400" in status, status
    print("download path-traversal rejected")
    s.close()

    print("WEB OK")


if __name__ == "__main__":
    main()
