"""UDP/WS 固件升级: 完整 START/DATA/END/REBOOT 换机流程 + 错误 keyhash 拒绝。

升级用当前同一镜像重刷: 版本不变, 验证完整换机链路
(擦写 ~1MB 外部 NOR + MCUboot swap + 重启, 全程 ~60-90s/次)。
"""
import base64
import json
import time

import pytest
import websocket

from helpers.device import parse_version, wait_http_json
from helpers.fwupd import FwUpg, keyhash_from_image, wait_online

pytestmark = [pytest.mark.functional, pytest.mark.upgrade]


def recv_ack(ws, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            ws.settimeout(max(1, deadline - time.time()))
            d = json.loads(ws.recv())
        except websocket.WebSocketTimeoutException:
            continue
        if "t" not in d:
            return d
    raise TimeoutError("ws fw ack timeout")


def test_upgrade_same_image(dev, fw_image):
    img = fw_image.read_bytes()
    keyhash_from_image(img)  # 镜像格式自检
    version_before = parse_version(dev.udp_xfer(b"\x04"))
    _, uptime_before = dev.http_json("GET", "/api/info")

    upg = FwUpg(dev)
    try:
        chunk = upg.start(img)
        assert chunk >= 512, chunk  # V2 大块通道可用
        off = upg.send_v2(img, chunk)
        assert off == len(img), (off, len(img))
        upg.end(img, test=False)
        upg.reboot()
    finally:
        upg.close()

    version_after = wait_online(dev, 120)
    assert version_after == version_before, (version_before, version_after)

    info = wait_http_json(dev)
    assert info["uptime_ms"] < 120_000, info["uptime_ms"]
    dev.tcp(21).close()  # 服务恢复


def test_upgrade_bad_keyhash_rejected(dev, fw_image):
    img = fw_image.read_bytes()
    upg = FwUpg(dev)
    try:
        bad = bytes([0x01]) + b"\x00\x00\x00\x00" + b"\xAA" * 32
        r = upg.xfer(bad, 10.0)
        assert r[0] == 0x01 and r[1] != 1, r.hex()  # keyhash 不匹配被拒
    finally:
        upg.close()
    assert parse_version(dev.udp_xfer(b"\x04"))  # 设备仍在线


def test_upgrade_over_ws(dev, fw_image):
    """SPA 同款升级通道: ws fw_start -> 二进制帧 -> fw_end -> 自动换机重启。"""
    img = fw_image.read_bytes()
    kh = base64.b64encode(keyhash_from_image(img)).decode()
    version_before = parse_version(dev.udp_xfer(b"\x04"))

    ws = websocket.create_connection(f"ws://{dev.ip}/ws", timeout=10)
    try:
        ws.send(json.dumps({"cmd": "fw_start", "size": len(img),
                            "keyhash": kh}))
        r = recv_ack(ws, 20)  # 擦除窗口
        if r.get("err") == "already in progress":
            ws.send(json.dumps({"cmd": "fw_end"}))
            recv_ack(ws, 10)
            ws.send(json.dumps({"cmd": "fw_start", "size": len(img),
                                "keyhash": kh}))
            r = recv_ack(ws, 20)
        assert r.get("ok") is True, r

        off = 0
        while off < len(img):
            ws.send_binary(img[off:off + 10240])
            off += 10240
        ws.send(json.dumps({"cmd": "fw_end"}))
        r = recv_ack(ws, 20)
        assert r.get("ok") is True, r
    finally:
        ws.close()

    # ws fw_end 走 ~3s 延迟重启 (heartbeat 优雅路径), 必须先等设备离线,
    # 否则 wait_online 会在重启前抢答, swap 落到后续测试头上
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            dev.udp_xfer(b"\x04", timeout=1.0)
            time.sleep(0.5)
        except OSError:
            break

    assert wait_online(dev, 120) == version_before
    info = wait_http_json(dev)
    assert info["uptime_ms"] < 120_000, info["uptime_ms"]
