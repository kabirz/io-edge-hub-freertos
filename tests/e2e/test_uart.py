"""串口 shell (USART1): 提示符/命令/回显/鲁棒性。"""
import pytest

from helpers.uart import drain, read_until, send_line

pytestmark = [pytest.mark.functional, pytest.mark.serial]

PROMPT = b"io> "


@pytest.fixture(autouse=True)
def freertos_only(fw_kind):
    if fw_kind != "freertos":
        pytest.skip("io> shell is FreeRTOS-port specific "
                    "(Zephyr uses the stock Zephyr shell)")


def test_prompt_and_help(uart):
    drain(uart)
    uart.write(b"\n")
    read_until(uart, PROMPT, timeout=3)
    send_line(uart, "help")
    out = read_until(uart, PROMPT, timeout=3)
    for cmd in (b"help", b"tasks", b"reboot", b"io"):
        assert cmd in out, (cmd, out)


def test_tasks(uart):
    drain(uart)
    send_line(uart, "tasks")
    out = read_until(uart, PROMPT, timeout=3)
    assert b"hb" in out or b"ftp" in out, out


def test_io_info_version(uart):
    drain(uart)
    send_line(uart, "io info")
    out = read_until(uart, PROMPT, timeout=3)
    assert b"v0." in out, out


def test_unknown_command(uart):
    drain(uart)
    send_line(uart, "zzz")
    out = read_until(uart, PROMPT, timeout=3)
    assert b"unknown command" in out, out


def test_garbage_line(uart):
    drain(uart)
    garbage = "".join(chr(33 + (i * 7) % 90) for i in range(80))
    send_line(uart, garbage)
    read_until(uart, PROMPT, timeout=3)  # shell 未崩溃
    send_line(uart, "help")
    assert b"tasks" in read_until(uart, PROMPT, timeout=3)


def test_long_line_truncated(uart):
    drain(uart)
    send_line(uart, "a" * 300)
    out = read_until(uart, PROMPT, timeout=3)
    assert b"unknown command" in out, out
