"""历史记录: 采样落盘/多视图一致性/记录格式/启停续写/文件删除。"""
import ftplib
import io
import json
import re
import struct
import time

import pytest

from helpers.device import read_http_message

pytestmark = pytest.mark.functional


def hist_set(dev, value):
    st, r = dev.http_json("POST", "/api/reg",
                          json.dumps({"addr": 5, "value": value}))
    assert "200" in st and r["ok"] is True, (st, r)


def hist_files(dev):
    _, h = dev.http_json("GET", "/api/history")
    return h["files"]


def hist_download(dev, name):
    s = dev.tcp(80)
    try:
        s.sendall(f"GET /api/history/download?name={name} HTTP/1.1\r\n"
                  f"Host: {dev.ip}\r\n\r\n".encode())
        st, _, body = read_http_message(s)
        assert "200" in st, (st, body[:120])
        return body
    finally:
        s.close()


def parse_records(data):
    """data_* 文件 = 小端记录流: DI(type1)=10B, AI(type2)=16B。"""
    off = 0
    records = 0
    while off < len(data):
        assert off + 6 <= len(data), f"torn record header at {off}"
        rtype, ts = struct.unpack_from("<HI", data, off)
        size = 10 if rtype == 1 else 16 if rtype == 2 else 0
        assert size, f"bad record type {rtype} at {off}"
        assert off + size <= len(data), f"torn record body at {off}"
        assert 946684800 <= ts <= 4102444800, ts  # 2000..2100
        off += size
        records += 1
    assert records > 0
    return records


@pytest.fixture(scope="module", autouse=True)
def history_enabled(dev):
    _, info = dev.http_json("GET", "/api/info")
    orig = info["hist_en"]
    hist_set(dev, 1)
    yield
    hist_set(dev, orig)


def wait_for_file(dev, timeout=25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        files = hist_files(dev)
        if files and files[0]["size"] > 200:
            return files[0]
        time.sleep(1)
    pytest.fail(f"no growing history file within {timeout}s: "
                f"{hist_files(dev)}")


def test_file_present_and_growing(dev):
    f1 = wait_for_file(dev)
    assert re.match(r"^data_[0-9A-Za-z._-]+\.raw$", f1["name"]), f1
    time.sleep(4)
    f2 = wait_for_file(dev)
    assert f2["name"] == f1["name"], (f1, f2)  # 同一文件持续追加
    assert f2["size"] > f1["size"], (f1, f2)


def test_web_download_consistent(dev):
    files = hist_files(dev)
    assert files
    newest = files[0]
    body = hist_download(dev, newest["name"])
    assert len(body) >= newest["size"], (len(body), newest)  # 下载前先 sync
    parse_records(body)


def test_ftp_view_consistent(dev, ftp):
    files = hist_files(dev)
    assert files
    newest = files[0]
    buf = io.BytesIO()
    ftp.retrbinary(f"RETR {newest['name']}", buf.write)
    assert buf.tell() >= newest["size"], (buf.tell(), newest)
    parse_records(buf.getvalue())


def test_disable_resume_same_file(dev):
    newest = wait_for_file(dev)
    hist_set(dev, 0)
    try:
        time.sleep(3)
        s1 = hist_files(dev)[0]["size"]
        time.sleep(3)
        s2 = hist_files(dev)[0]["size"]
        assert s1 == s2, "history still growing while disabled"
    finally:
        hist_set(dev, 1)
    # 重新使能必须续写同一文件 (而非新建)
    deadline = time.time() + 25
    while time.time() < deadline:
        files = hist_files(dev)
        if files and files[0]["size"] > s2:
            break
        time.sleep(1)
    assert files[0]["name"] == newest["name"], (newest, files[0])
    assert files[0]["size"] > s2, (s2, files[0])


def test_delete_fake_history_file(dev, ftp):
    ftp.storbinary("STOR data_9901_010203.raw", io.BytesIO(b"\x01" * 32))
    try:
        names = [f["name"] for f in hist_files(dev)]
        assert "data_9901_010203.raw" in names, names
        st, r = dev.http_json("POST", "/api/history/delete",
                              json.dumps({"name": "data_9901_010203.raw"}))
        assert "200" in st and r["ok"] is True, (st, r)
        names = [f["name"] for f in hist_files(dev)]
        assert "data_9901_010203.raw" not in names, names
        with pytest.raises(ftplib.error_perm):
            ftp.size("data_9901_010203.raw")
    finally:
        try:
            ftp.delete("data_9901_010203.raw")
        except ftplib.error_perm:
            pass
