"""Capture boot log from COM9 after MCU reset."""
import serial
import subprocess
import time
import sys

STLINK = r"C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Utility\ST-LINK Utility\ST-LINK_CLI.exe"

port = serial.Serial('COM9', 115200, timeout=0.1)
port.reset_input_buffer()

# Reset MCU in background
subprocess.Popen([STLINK, '-c', 'SWD', 'SWCLK=4000', '-Rst'],
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

data = bytearray()
deadline = time.time() + 10
last_recv = time.time()

while time.time() < deadline:
    n = port.in_waiting
    if n > 0:
        data.extend(port.read(n))
        last_recv = time.time()
    elif data and (time.time() - last_recv > 3):
        break  # 3s silence after getting data = done
    time.sleep(0.01)

port.close()

if data:
    text = data.decode('ascii', errors='replace')
    print(text)
else:
    print("NO DATA RECEIVED")
