"""History download flow probe: enable -> list -> download -> verify -> delete -> disable."""
import hashlib
import json
import socket
import sys
import time

sys.path.insert(0, "tools")
from web_probe import connect, req, recv_response


def main():
    ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.12.101"
    src = sys.argv[2] if len(sys.argv) > 2 else None
    global IP, SRC
    import web_probe
    web_probe.IP = ip

    s0 = connect()
    # enable history (holding 0x05)
    st, _, b = req(s0, "POST", "/api/reg", '{"addr":5,"value":1}')
    assert json.loads(b)["ok"], b
    print("history enabled, waiting for samples...")
    s0.close()

    fname = None
    for _ in range(20):
        time.sleep(1)
        s0 = connect()
        st, _, b = req(s0, "GET", "/api/history")
        files = json.loads(b)["files"]
        s0.close()
        if files and files[0]["size"] > 200:
            fname = files[0]["name"]
            fsize = files[0]["size"]
            print(f"file: {fname} ({fsize}B)")
            break
    assert fname, "no history file appeared"

    # raw download on a dedicated connection (Connection: close)
    s1 = connect()
    s1.sendall(f"GET /api/history/download?name={fname} HTTP/1.1\r\nHost: {ip}\r\n\r\n".encode())
    status, head, body = recv_response(s1)
    assert "200" in status, status
    assert f"filename=\"{fname}\"".encode() in head.encode() or fname in head, head
    assert len(body) == fsize, (len(body), fsize)
    print(f"downloaded {len(body)}B, md5={hashlib.md5(body).hexdigest()[:12]}")
    s1.close()

    # list size should grow while enabled; then delete
    time.sleep(2)
    s2 = connect()
    st, _, b = req(s2, "POST", "/api/history/delete", json.dumps({"name": fname}))
    assert json.loads(b)["ok"], b
    st, _, b = req(s2, "GET", "/api/history")
    names = [f["name"] for f in json.loads(b)["files"]]
    assert fname not in names, names
    print("delete ok, list:", names)
    # disable history + save
    st, _, b = req(s2, "POST", "/api/reg", '{"addr":5,"value":0}')
    assert json.loads(b)["ok"], b
    st, _, b = req(s2, "POST", "/api/save", "{}")
    assert json.loads(b)["ok"], b
    s2.close()
    print("HISTORY-WEB OK")


if __name__ == "__main__":
    main()
