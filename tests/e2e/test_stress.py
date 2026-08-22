"""压力测试: 连接风暴/高频轮询/并发传输/大文件/混合负载/事后健康。"""
import hashlib
import io
import os
import socket
import threading
import time
import statistics

import pytest
import websocket

from helpers.device import http_on, read_http_message
from helpers.mbtcp import MbTcp, adu, regs

pytestmark = pytest.mark.stress


@pytest.fixture(scope="module", autouse=True)
def sampling_enabled(dev):
    _, info = dev.http_json("GET", "/api/info")
    orig = info["hist_en"]
    dev.http("POST", "/api/reg", '{"addr":5,"value":1}')
    yield
    dev.http("POST", "/api/reg", '{"addr":5,"value":%d}' % orig)


def pct(latencies, p):
    data = sorted(latencies)
    return data[min(len(data) - 1, int(len(data) * p / 100))]


def test_http_churn_300(dev):
    lat = []
    for _ in range(300):
        t0 = time.time()
        st, _, _ = dev.http("GET", "/api/info")
        assert "200" in st, st
        lat.append(time.time() - t0)
    print(f"http churn: n=300 p50={pct(lat,50)*1000:.1f}ms "
          f"p95={pct(lat,95)*1000:.1f}ms max={max(lat)*1000:.1f}ms")


def test_http_keepalive_burst_300(dev):
    s = dev.tcp(80)
    lat = []
    try:
        for _ in range(300):
            t0 = time.time()
            st, _, _ = http_on(s, "GET", "/api/io", host=dev.ip)
            assert "200" in st, st
            lat.append(time.time() - t0)
    finally:
        s.close()
    print(f"keepalive burst: n=300 p50={pct(lat,50)*1000:.1f}ms "
          f"p95={pct(lat,95)*1000:.1f}ms max={max(lat)*1000:.1f}ms")


def test_ftp_connect_churn_100(dev):
    for _ in range(100):
        s = dev.tcp(21, timeout=5)
        try:
            assert s.recv(256).startswith(b"220 ")
        finally:
            s.close()


def test_modbus_burst_500(dev):
    with MbTcp(dev) as m:
        lat = []
        for i in range(500):
            t0 = time.time()
            val = regs(m.req(MbTcp.read_req(3, 0, 1), tid=(i + 1) & 0xFFFF))
            assert len(val) == 1
            lat.append(time.time() - t0)
    print(f"modbus burst: n=500 p50={pct(lat,50)*1000:.1f}ms "
          f"p95={pct(lat,95)*1000:.1f}ms max={max(lat)*1000:.1f}ms")


def test_modbus_pipelined_burst_100(dev):
    with MbTcp(dev) as m:
        for _ in range(100):
            m.sock.sendall(adu(1, 1, MbTcp.read_req(3, 0, 1))
                           + adu(2, 1, MbTcp.read_req(4, 0, 1)))
            buf = b""
            replies = []
            m.sock.settimeout(5)
            while len(replies) < 2:
                chunk = m.sock.recv(260)
                if not chunk:
                    raise ConnectionError("closed during pipelined burst")
                buf += chunk
                while len(buf) >= 6:
                    n = 6 + ((buf[4] << 8) | buf[5])
                    if len(buf) < n:
                        break
                    replies.append(buf[:n])
                    buf = buf[n:]
            assert replies[0][7] == 3 and replies[1][7] == 4


def test_udp_burst_300(dev):
    replied = 0
    s = dev.udp(timeout=2.0)
    try:
        for _ in range(300):
            s.sendto(b"\x04", (dev.ip, 8600))
            try:
                r, _ = s.recvfrom(512)
                if r[0] == 0x04:
                    replied += 1
            except socket.timeout:
                pass
    finally:
        s.close()
    assert replied >= 295, replied  # 允许 <=2% 丢包


def _ftp_roundtrip(dev, name, size, results, idx):
    try:
        payload = os.urandom(size)
        f = dev.ftp(timeout=30)
        try:
            f.storbinary(f"STOR {name}", io.BytesIO(payload))
            buf = io.BytesIO()
            f.retrbinary(f"RETR {name}", buf.write)
            assert hashlib.md5(buf.getvalue()).hexdigest() == \
                hashlib.md5(payload).hexdigest()
            f.delete(name)
        finally:
            f.quit()
        results[idx] = True
    except Exception as e:
        results[idx] = f"{type(e).__name__}: {e}"


def test_ftp_three_clients_parallel(dev, ftp_creds):
    results = [None] * 3
    threads = [threading.Thread(target=_ftp_roundtrip,
                                args=(dev, f"t_stress_{i}.bin", 128 * 1024,
                                      results, i))
               for i in range(3)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=120)
    assert all(t.is_alive() is False for t in threads), "ftp client hung"
    for i, r in enumerate(results):
        assert r is True, f"client {i}: {r}"
    # 3 会话占用期间第 4 个必须被拒
    hold = [dev.ftp() for _ in range(3)]
    try:
        with pytest.raises(Exception):
            dev.ftp()
    finally:
        for f in hold:
            f.quit()


def test_ftp_large_file_1mb(dev):
    payload = os.urandom(1024 * 1024)
    t0 = time.time()
    f = dev.ftp(timeout=60)
    try:
        f.storbinary("STOR t_big.bin", io.BytesIO(payload))
        t_stor = time.time() - t0
        assert f.size("t_big.bin") == len(payload)
        buf = io.BytesIO()
        f.retrbinary("RETR t_big.bin", buf.write)
        t_retr = time.time() - t0 - t_stor
        assert buf.getvalue() == payload
    finally:
        try:
            f.delete("t_big.bin")
        except Exception:
            pass
        f.quit()
    print(f"ftp 1MB: STOR {len(payload)/t_stor/1024:.0f}KB/s "
          f"RETR {len(payload)/t_retr/1024:.0f}KB/s")


def test_mixed_workload_30s(dev):
    stop = time.time() + 30
    stats = {"http": [0, 0], "mb": [0, 0], "udp": [0, 0],
             "ftp": [0, 0], "ws_frames": 0}
    errors = []

    def http_poll():
        while time.time() < stop:
            try:
                st, _, _ = dev.http("GET", "/api/io", timeout=3)
                assert "200" in st, st
                stats["http"][0] += 1
            except Exception as e:
                stats["http"][1] += 1
                errors.append(f"http: {e!r}")
            time.sleep(0.15)

    def mb_poll():
        m = MbTcp(dev, timeout=3)
        try:
            while time.time() < stop:
                try:
                    regs(m.req(MbTcp.read_req(3, 0, 4)))
                    stats["mb"][0] += 1
                except Exception as e:
                    stats["mb"][1] += 1
                    errors.append(f"mb: {e!r}")
                time.sleep(0.1)
        finally:
            m.close()

    def udp_poll():
        while time.time() < stop:
            try:
                r = dev.udp_xfer(b"\x04", timeout=2)
                assert r[0] == 0x04
                stats["udp"][0] += 1
            except Exception as e:
                stats["udp"][1] += 1
                errors.append(f"udp: {e!r}")
            time.sleep(0.3)

    def ftp_loop():
        i = 0
        while time.time() < stop:
            try:
                _ftp_roundtrip(dev, f"t_mix_{i % 2}.bin", 64 * 1024,
                               [True], 0)
                stats["ftp"][0] += 1
            except Exception as e:
                stats["ftp"][1] += 1
                errors.append(f"ftp: {e!r}")
            i += 1

    def ws_recv():
        try:
            ws = websocket.create_connection(f"ws://{dev.ip}/ws",
                                             timeout=5)
            while time.time() < stop:
                try:
                    ws.settimeout(2)
                    ws.recv()
                    stats["ws_frames"] += 1
                except websocket.WebSocketTimeoutException:
                    continue
            ws.close()
        except Exception as e:
            errors.append(f"ws: {e!r}")

    _, hist0 = dev.http_json("GET", "/api/history")
    size0 = hist0["files"][0]["size"] if hist0["files"] else 0

    threads = [threading.Thread(target=t)
               for t in (http_poll, mb_poll, udp_poll, ftp_loop, ws_recv)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=45)

    _, info = dev.http_json("GET", "/api/info")
    _, hist1 = dev.http_json("GET", "/api/history")
    size1 = hist1["files"][0]["size"] if hist1["files"] else 0

    print(f"mixed: http={stats['http']} mb={stats['mb']} "
          f"udp={stats['udp']} ftp={stats['ftp']} ws={stats['ws_frames']}")
    assert stats["http"][1] == 0, errors
    assert stats["mb"][1] == 0, errors
    assert stats["udp"][0] >= 80, stats["udp"]  # 30s @0.3s 允许少量超时
    assert stats["ftp"][1] == 0, errors
    assert stats["ws_frames"] >= 10, stats["ws_frames"]
    assert size1 > size0, (size0, size1)  # 高负载下采样不中断
    assert info["net_up"] is True and info["uptime_ms"] > 25_000, info


def test_health_after_stress(dev):
    from helpers.device import parse_version
    assert parse_version(dev.udp_xfer(b"\x04"))
    st, info = dev.http_json("GET", "/api/info")
    assert "200" in st and info["net_up"] is True
    f = dev.ftp()
    try:
        f.retrlines("NLST", lambda line: None)
    finally:
        f.quit()
    with MbTcp(dev) as m:
        regs(m.req(MbTcp.read_req(3, 0, 1)))
    _, h = dev.http_json("GET", "/api/history")
    assert h["files"] and h["files"][0]["size"] > 0
