import ftplib
from pathlib import Path

import pytest
import serial

from helpers.device import Device, autodetect_src, parse_version
from helpers.mbtcp import MbTcp

REPO_ROOT = Path(__file__).resolve().parents[2]


def pytest_addoption(parser):
    parser.addoption("--ip", default="192.168.12.101", help="device IP")
    parser.addoption("--src-ip", default=None,
                     help="local source IP to bind (default: auto-detect "
                          "the NIC in the device's /24, bypassing TUN proxy)")
    parser.addoption("--net-timeout", type=float, default=5.0)
    parser.addoption("--ftp-user", default="admin")
    parser.addoption("--ftp-pass", default="admin")
    parser.addoption("--serial", default="COM9",
                     help="device log/shell UART port ('' to disable)")
    parser.addoption("--baud", type=int, default=115200)
    parser.addoption("--rs485-port", default=None,
                     help="USB-RS485 adapter COM port for Modbus RTU tests")
    parser.addoption("--rs485-baud", type=int, default=0,
                     help="RS485 baud (0 = follow device config)")
    parser.addoption("--fw-image", default=None,
                     help="signed image for the upgrade test "
                          "(default: build/app.signed.bin)")


@pytest.fixture(scope="session")
def dev(request):
    ip = request.config.getoption("--ip")
    src = request.config.getoption("--src-ip") or autodetect_src(ip)
    d = Device(ip, src, request.config.getoption("--net-timeout"))
    try:
        v = parse_version(d.udp_xfer(b"\x04", timeout=3.0))
    except Exception as e:
        pytest.fail(f"device {ip} unreachable via UDP :8600 ({e!r}); "
                    f"check --ip / --src-ip")
    return d


@pytest.fixture(scope="session")
def ftp_creds(request):
    return request.config.getoption("--ftp-user"), \
        request.config.getoption("--ftp-pass")


@pytest.fixture
def ftp(dev, ftp_creds):
    user, password = ftp_creds
    f = dev.ftp(user, password)
    try:
        yield f
    finally:
        try:
            f.quit()
        except Exception:
            f.close()


@pytest.fixture
def mb(dev):
    with MbTcp(dev) as client:
        yield client


@pytest.fixture
def uart(request):
    port = request.config.getoption("--serial")
    if not port:
        pytest.skip("--serial empty: UART tests disabled")
    try:
        ser = serial.Serial(port, request.config.getoption("--baud"),
                            timeout=0.1)
    except serial.SerialException as e:
        pytest.skip(f"UART {port} unavailable: {e}")
    try:
        yield ser
    finally:
        ser.close()


@pytest.fixture(scope="session")
def fw_image(request):
    p = request.config.getoption("--fw-image")
    if p is None:
        p = REPO_ROOT / "build" / "app.signed.bin"
    p = Path(p)
    if not p.exists():
        pytest.skip(f"fw image not found: {p}")
    return p
