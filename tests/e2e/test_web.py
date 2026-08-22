"""Web 服务功能测试: 静态页/REST API/JSON 校验/解析边界/连接限制。"""
import gzip
import json
import re
import socket
import time

import pytest

from helpers.device import http_on, read_http_messages

pytestmark = pytest.mark.functional


def http_json_ok(dev, method, path, body=None):
    st, r = dev.http_json(method, path, body)
    assert "200" in st and r.get("ok") is True, (st, r)
    return r


def http_err(dev, method, path, body=None, want="400"):
    st, r = dev.http_json(method, path, body)
    assert want in st, (st, r)
    return r


def test_index_page_gzip(dev):
    st, headers, body = dev.http("GET", "/")
    assert "200" in st, st
    assert headers.get("content-encoding") == "gzip", headers
    page = gzip.decompress(body)
    assert b"<html" in page.lower() and len(page) > 20000, len(page)


def test_keepalive_two_requests(dev):
    s = dev.tcp(80)
    try:
        st, _, _ = http_on(s, "GET", "/api/io", host=dev.ip)
        assert "200" in st, st
        st, _, _ = http_on(s, "GET", "/api/io", host=dev.ip)
        assert "200" in st, st
    finally:
        s.close()


def test_api_io_shape(dev):
    _, io = dev.http_json("GET", "/api/io")
    assert io["t"] == "io"
    assert len(io["di"]) == 16 and all(v in (0, 1) for v in io["di"])
    assert len(io["do"]) == 8 and all(v in (0, 1) for v in io["do"])
    assert len(io["ai"]) == 4


def test_api_regs_shape(dev):
    _, regs = dev.http_json("GET", "/api/regs")
    assert regs["t"] == "regs"
    assert len(regs["holding"]) == 18
    assert len(regs["input"]) == 6
    assert 0 <= regs["holding"][0] <= 0xFF  # DO 位掩码


def test_do_roundtrip(dev):
    try:
        http_json_ok(dev, "POST", "/api/do", '{"index":0,"value":1}')
        _, io = dev.http_json("GET", "/api/io")
        assert io["do"][0] == 1, io["do"]
    finally:
        http_json_ok(dev, "POST", "/api/do", '{"index":0,"value":0}')
    _, io = dev.http_json("GET", "/api/io")
    assert io["do"][0] == 0, io["do"]


def test_do_invalid(dev):
    for body in ('{"index":8,"value":1}', '{"index":-1,"value":1}',
                 '{"value":1}', '{"index":"x","value":1}'):
        r = http_err(dev, "POST", "/api/do", body)
        assert r.get("ok") is False, (body, r)


def test_reg_invalid(dev):
    for body in ('{"addr":18,"value":1}', '{"addr":-1,"value":1}',
                 '{"addr":0,"value":65536}', '{"addr":0}'):
        r = http_err(dev, "POST", "/api/reg", body)
        assert r.get("ok") is False, (body, r)


def test_time_endpoint(dev):
    http_err(dev, "POST", "/api/time", '{"ts":0}')
    http_err(dev, "POST", "/api/time", '{"ts":946684799}')  # TS_MIN-1
    http_json_ok(dev, "POST", "/api/time",
                 json.dumps({"ts": int(time.time())}))


def test_cfg_validation(dev):
    _, info0 = dev.http_json("GET", "/api/info")
    orig_sid = info0["slave_id"]
    try:
        http_json_ok(dev, "POST", "/api/cfg", json.dumps({"ip": dev.ip}))
        _, info = dev.http_json("GET", "/api/info")
        assert info["ip"] == dev.ip

        http_json_ok(dev, "POST", "/api/cfg", '{"sid":2}')
        _, info = dev.http_json("GET", "/api/info")
        assert info["slave_id"] == 2
        http_json_ok(dev, "POST", "/api/cfg", json.dumps({"sid": orig_sid}))

        cases = [('{"ip":"999.1.1.1"}', "invalid ip"),
                 ('{"ip":"1.2.3"}', "invalid ip"),
                 ('{"ip":"256.0.0.0"}', "invalid ip"),
                 ('{"ip":"192.168.12.0"}', "invalid ip"),
                 ('{"ip":"192.168.12.255"}', "invalid ip"),
                 ('{"ip":"abc"}', "invalid ip"),
                 ('{"rs485":100}', "invalid rs485 baud"),
                 ('{"sid":0}', "invalid slave id"),
                 ('{"sid":248}', "invalid slave id"),
                 ('{"can_bps":300}', "invalid can baud"),
                 ('{"can_id":0}', "invalid can id"),
                 ('{"can_id":2048}', "invalid can id")]
        for body, want_err in cases:
            st, r = dev.http_json("POST", "/api/cfg", body)
            assert "400" in st and want_err in r.get("err", ""), (body, st, r)
    finally:
        dev.http("POST", "/api/cfg", json.dumps({"sid": orig_sid}))
    _, info = dev.http_json("GET", "/api/info")
    assert info["slave_id"] == orig_sid


def test_save(dev):
    http_json_ok(dev, "POST", "/api/save", "{}")


def test_history_download_invalid_name(dev):
    for name in ("../etc/passwd", "abc", "data_../../x", "no_data_1.raw"):
        st, r = dev.http_json(
            "GET", f"/api/history/download?name={name}")
        assert "400" in st and r.get("ok") is False, (name, st, r)


def test_404_and_method(dev, fw_kind):
    st, _, _ = dev.http("GET", "/api/nonexistent")
    assert "404" in st, st
    if fw_kind == "freertos":  # JSON 错误体是移植版自有增强
        _, r = dev.http_json("GET", "/api/nonexistent")
        assert r.get("err") == "not found", r
    st, _, _ = dev.http("DELETE", "/api/io")
    assert "405" in st, st


def test_body_too_large(dev):
    st, _, body = dev.http("POST", "/api/do", '{"index":1,"value":1,"pad":"' +
                           "x" * 150 + '"}')
    assert "400" in st and json.loads(body) == {
        "ok": False, "err": "body too large"}, (st, body[:120])


def test_request_line_parser_edges(dev, fw_kind):
    def one_shot(req):
        s = dev.tcp(80)
        try:
            s.sendall(req)
            msgs = read_http_messages(s, 1)
            assert msgs, "no response"
            return msgs[0][0]
        finally:
            s.close()

    assert "200" in one_shot(b"GET  /api/io HTTP/1.1\r\nHost: x\r\n\r\n")
    st = one_shot(b"GET\t/api/io HTTP/1.1\r\nHost: x\r\n\r\n")  # Tab 分隔
    if fw_kind == "freertos":
        assert "200" in st, st
    else:
        assert st.split()[1] != "200", st  # Zephyr 解析器拒绝 Tab
    assert "200" in one_shot(
        b"GET /api/history?x=1&y=2 HTTP/1.1\r\nHost: x\r\n\r\n")
    long_path = "/" + "a" * 200
    assert "404" in one_shot(
        f"GET {long_path} HTTP/1.1\r\nHost: x\r\n\r\n".encode())


def test_pipelined_requests(dev, fw_kind):
    if fw_kind != "freertos":
        pytest.skip("Zephyr http lib rejects same-segment pipelining")
    s = dev.tcp(80)
    try:
        req = (f"GET /api/io HTTP/1.1\r\nHost: {dev.ip}\r\n\r\n"
               f"GET /api/regs HTTP/1.1\r\nHost: {dev.ip}\r\n\r\n")
        s.sendall(req.encode())
        msgs = read_http_messages(s, 2)
        assert len(msgs) == 2, len(msgs)
        assert "200" in msgs[0][0] and b'"t":"io"' in msgs[0][2]
        assert "200" in msgs[1][0] and b'"t":"regs"' in msgs[1][2]
    finally:
        s.close()


def test_max_two_connections(dev, fw_kind):
    if fw_kind != "freertos":
        pytest.skip("connection cap is an httpd-port policy "
                    "(Zephyr http lib serves more)")
    c1, c2 = dev.tcp(80), dev.tcp(80)
    try:
        with pytest.raises((ConnectionError, TimeoutError, socket.timeout)):
            c3 = dev.tcp(80)
            try:
                c3.sendall(f"GET /api/io HTTP/1.1\r\nHost: x\r\n\r\n"
                           .encode())
                c3.settimeout(3)
                data = c3.recv(4096)
                assert not (data and b"200" in data), \
                    f"3rd connection served: {data[:60]!r}"
            finally:
                c3.close()
    finally:
        c1.close()
        c2.close()
