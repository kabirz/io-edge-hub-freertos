"""Shell/UART interaction helpers."""
import time


def read_until(ser, pattern, timeout):
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        n = ser.in_waiting
        if n:
            buf.extend(ser.read(n))
            if pattern in buf:
                return bytes(buf)
        time.sleep(0.02)
    raise TimeoutError(
        f"{pattern!r} not seen within {timeout}s; tail={bytes(buf[-200:])!r}")


def send_line(ser, line):
    ser.write(line.encode() + b"\r\n")


def drain(ser):
    if ser.in_waiting:
        ser.read(ser.in_waiting)
