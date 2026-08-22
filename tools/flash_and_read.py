"""Flash MCU and read COM9 simultaneously to verify VCOM link."""
import subprocess
import serial
import threading
import time

STLINK = r"C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Utility\ST-LINK Utility\ST-LINK_CLI.exe"
HEX = r"C:\Users\jxwaz\code\io-edge-hub-freertos\build\app.hex"

collected = bytearray()

def read_serial():
    """Read COM9 in background thread."""
    try:
        p = serial.Serial('COM9', 115200, timeout=0.5)
        deadline = time.time() + 20
        while time.time() < deadline:
            n = p.in_waiting
            if n > 0:
                chunk = p.read(n)
                collected.extend(chunk)
            time.sleep(0.05)
        p.close()
    except Exception as e:
        print(f"COM9 error: {e}")

# Start reading COM9 first
t = threading.Thread(target=read_serial, daemon=True)
t.start()
time.sleep(0.5)  # Let serial reader settle

# Now flash and reset
print("Flashing...")
result = subprocess.run(
    [STLINK, '-c', 'SWD', 'SWCLK=4000', '-P', HEX, '-V', '-Rst'],
    capture_output=True, timeout=30
)

# Wait for serial reader to finish
t.join(timeout=25)

print(f"\nCollected {len(collected)} bytes from COM9")
if collected:
    # Skip any 'A' prefix from VCOM buffer residue
    data = collected.decode('ascii', errors='replace')
    # Find first '[' which starts our log format
    idx = data.find('[')
    if idx >= 0:
        print("=== Boot log ===")
        print(data[idx:])
    else:
        print(data[:2000])
else:
    print("NO DATA")
