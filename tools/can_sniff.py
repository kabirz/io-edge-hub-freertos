"""Sniff ALL CAN frames for N seconds while resetting the device."""
import subprocess
import sys
import time

import can

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 4.0
STLINK = (r"C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Utility"
          r"\ST-LINK Utility\ST-LINK_CLI.exe")

bus = can.Bus(interface='pcan', channel='PCAN_USBBUS1', bitrate=250000)
subprocess.run([STLINK, '-c', 'SWD', 'SWCLK=4000', '-Rst'],
               capture_output=True)
t0 = time.monotonic()
n = 0
while time.monotonic() - t0 < SECS:
    m = bus.recv(timeout=0.2)
    if m is not None:
        n += 1
        print('%7.3f  0x%03X  %s' % (time.monotonic() - t0, m.arbitration_id,
                                     bytes(m.data).hex()))
print('total %d frames' % n)
bus.shutdown()
