"""WebSocket end-to-end test: push frames, commands, full fw upgrade
v0.3.12 -> v0.3.13 over WS binary frames (SPA parity: 10KB chunks),
with COM9 capture as swap evidence."""
import base64
import json
import struct
import threading
import time

import serial
import websocket  # websocket-client

IP = '192.168.12.101'
IMG = r'C:\Users\jxwaz\code\io-edge-hub-freertos\build\test-v0.3.13.bin'

collected = bytearray()
stop = False


def reader():
    try:
        p = serial.Serial('COM9', 115200, timeout=0.2)
        while not stop:
            n = p.in_waiting
            if n:
                collected.extend(p.read(n))
            time.sleep(0.02)
        p.close()
    except Exception as e:
        print('COM9 err:', e)


threading.Thread(target=reader, daemon=True).start()
time.sleep(0.5)

img = open(IMG, 'rb').read()

# keyhash from TLV
magic, hdr_size, img_size = struct.unpack_from('<I4xH2xI', img, 0)
assert magic == 0x96F3B83D, hex(magic)
tlv_off = hdr_size + img_size
tlv_magic, tlv_size = struct.unpack_from('<HH', img, tlv_off)
assert tlv_magic == 0x6907
off, kh = tlv_off + 4, None
while off + 4 <= tlv_off + tlv_size:
    tag, ln = struct.unpack_from('<HH', img, off)
    if tag == 0x0001 and ln == 32:
        kh = img[off + 4:off + 36]
        break
    off += 4 + (ln + 3) & ~3
assert kh is not None
print('image %d B, keyhash %s' % (len(img), kh.hex()))

ws = websocket.create_connection('ws://%s/ws' % IP, timeout=5)
print('ws connected')


def recv_ack(timeout=10):
    """收帧直到 ack (无 "t" 字段); 推送帧丢弃。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            ws.settimeout(max(1, deadline - time.time()))
            d = json.loads(ws.recv())
        except websocket.WebSocketTimeoutException:
            continue
        except Exception as e:
            raise RuntimeError('ws closed: %s' % e)
        if 't' not in d:
            return d
    raise RuntimeError('ack timeout')

# ---- 1. push frames (io/regs within ~2s) ----
seen = set()
deadline = time.time() + 4
while time.time() < deadline and len(seen) < 3:
    try:
        ws.settimeout(2)
        d = json.loads(ws.recv())
    except websocket.WebSocketTimeoutException:
        break
    if 't' in d:
        seen.add(d['t'])
print('push frames seen:', sorted(seen))
assert 'io' in seen and 'regs' in seen, 'push frames missing'

# ---- 2. command + ack (write holding reg 0 = DI filter, harmless) ----
ws.send(json.dumps({'cmd': 'reg', 'addr': 0, 'value': 0}))
r = recv_ack(5)
print('reg ack:', r)
assert r.get('ok') is True

# ---- 3. fw upgrade over WS (SPA flow) ----
ws.send(json.dumps({'cmd': 'fw_start', 'size': len(img),
                    'keyhash': base64.b64encode(kh).decode()}))
r = recv_ack(15)  # 擦除窗口
if r.get('err') == 'already in progress':
    print('stuck session, clearing via fw_end...')
    ws.send(json.dumps({'cmd': 'fw_end'}))
    recv_ack(10)
    ws.send(json.dumps({'cmd': 'fw_start', 'size': len(img),
                        'keyhash': base64.b64encode(kh).decode()}))
    r = recv_ack(15)
print('fw_start ack:', r)
assert r.get('ok') is True, r

t0 = time.time()
CHUNK = 10240
off = 0
while off < len(img):
    end = min(off + CHUNK, len(img))
    ws.send_binary(img[off:end])
    off = end
    time.sleep(0.005)
ws.send(json.dumps({'cmd': 'fw_end'}))
r = recv_ack(15)
print('fw_end ack:', r, '(%.0f B/s)' % (len(img) / max(time.time() - t0, 1e-3)))
assert r.get('ok') is True, r

# ---- 4. swap + reboot window ----
time.sleep(30)
import urllib.request
try:
    info = urllib.request.urlopen('http://%s/api/info' % IP, timeout=5).read()
    print('after swap /api/info:', info.decode()[:200])
except Exception as e:
    print('info poll failed (device rebooting?):', e)

time.sleep(2)
stop = True
time.sleep(0.5)
txt = collected.decode('ascii', errors='replace')
print('=== COM9 ===')
print(txt[-1800:])
