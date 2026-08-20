import serial
import time

p = serial.Serial('COM9', 115200, timeout=0.5)
p.reset_input_buffer()
data = bytearray()
deadline = time.time() + 8
while time.time() < deadline:
    n = p.in_waiting
    if n > 0:
        data.extend(p.read(n))
    time.sleep(0.05)
p.close()
print(f'Got {len(data)} bytes')
if data:
    print(data.decode('ascii', 'replace'))
else:
    print('STILL NO DATA')
