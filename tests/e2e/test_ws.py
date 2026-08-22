"""WebSocket: 握手/推送帧/命令 ack/单会话限制。"""
import json
import time

import pytest
import websocket

pytestmark = pytest.mark.functional


def ws_connect(dev, timeout=5):
    return websocket.create_connection(f"ws://{dev.ip}/ws", timeout=timeout)


def recv_json(ws, timeout):
    ws.settimeout(timeout)
    return json.loads(ws.recv())


def test_ws_push_and_cmd(dev):
    ws = ws_connect(dev)
    try:
        seen = set()
        deadline = time.time() + 6
        while time.time() < deadline and seen != {"io", "regs"}:
            try:
                d = recv_json(ws, 2)
            except websocket.WebSocketTimeoutException:
                continue
            if "t" in d:
                seen.add(d["t"])
        assert "io" in seen and "regs" in seen, seen

        ws.send(json.dumps({"cmd": "reg", "addr": 0, "value": 0}))
        deadline = time.time() + 5
        ack = None
        while time.time() < deadline:
            d = recv_json(ws, 2)
            if "t" not in d:
                ack = d
                break
        assert ack and ack.get("ok") is True, ack
    finally:
        ws.close()


def test_ws_single_session(dev):
    ws = ws_connect(dev)
    try:
        s = dev.tcp(80)
        try:
            key_headers = ("GET /ws HTTP/1.1\r\n"
                           f"Host: {dev.ip}\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n")
            s.sendall(key_headers.encode())
            s.settimeout(5)
            data = s.recv(4096)
            assert b"503" in data and b"ws busy" in data, data[:120]
        finally:
            s.close()
    finally:
        ws.close()


def test_ws_close_frees_session(dev):
    ws = ws_connect(dev)
    ws.close()
    time.sleep(0.5)
    ws2 = ws_connect(dev)  # 立即可再次接入
    ws2.close()
