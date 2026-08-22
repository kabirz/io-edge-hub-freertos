"""重启路径 (web/UDP/串口): history 先落盘, 重启后续写同一文件, 服务恢复。

每条路径都会真实重启设备 (~25s/次); 采样使能作为前置条件被持久化,
测试结束恢复进入时的 hist_en 配置。
"""
import socket
import time

import pytest

from helpers.fwupd import wait_online
from helpers.uart import drain, read_until, send_line

pytestmark = [pytest.mark.functional, pytest.mark.reboot]


def newest_history(dev):
    _, h = dev.http_json("GET", "/api/history")
    return h["files"][0] if h["files"] else None


def set_hist_en(dev, value, save=False):
    st, r = dev.http_json("POST", "/api/reg",
                          '{"addr":5,"value":%d}' % value)
    assert "200" in st and r["ok"] is True, (st, r)
    if save:
        dev.http("POST", "/api/save", "{}")


def ensure_online(dev, timeout=60):
    wait_online(dev, timeout)


@pytest.fixture(scope="module", autouse=True)
def sampling_enabled(dev):
    _, info = dev.http_json("GET", "/api/info")
    orig = info["hist_en"]
    set_hist_en(dev, 1, save=True)  # 重启后仍需采样 (前置条件持久化)
    yield
    ensure_online(dev)
    set_hist_en(dev, orig, save=True)


def expect_reboot_and_recover(dev, trigger, offline_within=15):
    _, before = dev.http_json("GET", "/api/info")
    hist = newest_history(dev)

    trigger()

    deadline = time.time() + offline_within
    while time.time() < deadline:
        try:
            dev.udp_xfer(b"\x04", timeout=1.0)
            time.sleep(0.5)
        except OSError:
            break
    else:
        pytest.fail("device did not go offline after reboot request")

    wait_online(dev, 90)

    _, after = dev.http_json("GET", "/api/info")
    assert after["uptime_ms"] < 90_000, after["uptime_ms"]
    assert after["version"] == before["version"]

    dev.tcp(21).close()  # FTP 端口可达
    hist2 = newest_history(dev)
    assert hist2, "history file missing after reboot"
    if hist:
        assert hist2["name"] == hist["name"], (hist, hist2)
        assert hist2["size"] >= hist["size"], (hist, hist2)

    size2 = hist2["size"]
    deadline = time.time() + 20
    while time.time() < deadline:
        if newest_history(dev)["size"] > size2:
            break
        time.sleep(1)
    else:
        pytest.fail(f"sampling did not resume after reboot: {hist2}")


def test_web_reboot(dev):
    ensure_online(dev)

    def trigger():
        st, r = dev.http_json("POST", "/api/reboot", "{}")
        assert "200" in st and r["ok"] is True, (st, r)

    expect_reboot_and_recover(dev, trigger)


def test_udp_reboot(dev):
    ensure_online(dev)

    def trigger():
        r = dev.udp_xfer(bytes([0x05]))
        assert r == bytes([0x05, 0x01]), r.hex()

    expect_reboot_and_recover(dev, trigger)


def test_shell_reboot(dev, uart):
    ensure_online(dev)
    drain(uart)
    uart.write(b"\n")
    read_until(uart, b"io> ", timeout=5)

    def trigger():
        send_line(uart, "reboot")
        read_until(uart, b"reboot", timeout=3)

    expect_reboot_and_recover(dev, trigger, offline_within=25)
