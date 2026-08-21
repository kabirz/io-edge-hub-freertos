"""Flash boot+fw dual images (optionally mass-erase first) and capture COM9 log."""
import subprocess
import serial
import sys
import threading
import time

STLINK = r"C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Utility\ST-LINK Utility\ST-LINK_CLI.exe"
BUILD = r"C:\Users\jxwaz\code\io-edge-hub-freertos\build"

collected = bytearray()


def read_serial(seconds):
    try:
        p = serial.Serial('COM9', 115200, timeout=0.5)
        deadline = time.time() + seconds
        while time.time() < deadline:
            n = p.in_waiting
            if n > 0:
                collected.extend(p.read(n))
            time.sleep(0.05)
        p.close()
    except Exception as e:
        print(f"COM9 error: {e}")


def run(args, timeout=120):
    r = subprocess.run([STLINK] + args, capture_output=True, timeout=timeout)
    out = (r.stdout + r.stderr).decode('ascii', errors='replace')
    tail = out[-600:] if len(out) > 600 else out
    print(tail)
    return r.returncode


def main():
    mass_erase = '--me' in sys.argv
    t = threading.Thread(target=lambda: read_serial(35), daemon=True)
    t.start()
    time.sleep(0.5)

    if mass_erase:
        print("=== mass erase ===")
        rc = run(['-c', 'SWD', 'SWCLK=4000', '-ME'])
        if rc != 0:
            sys.exit(1)
    print("=== program boot ===")
    rc = run(['-c', 'SWD', 'SWCLK=4000', '-P', BUILD + r'\boot.hex', '-V'])
    if rc != 0:
        sys.exit(1)
    print("=== program fw ===")
    rc = run(['-c', 'SWD', 'SWCLK=4000', '-P', BUILD + r'\fw.hex', '-V', '-Rst'])
    if rc != 0:
        sys.exit(1)

    t.join(timeout=30)
    print(f"\n=== COM9: {len(collected)} bytes ===")
    data = collected.decode('ascii', errors='replace')
    print(data[-3000:])


if __name__ == '__main__':
    main()
