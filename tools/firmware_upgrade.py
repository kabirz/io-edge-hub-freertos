"""io-edge-hub-freertos firmware upgrade host tool (UDP channel).

Protocol matches the Zephyr io-edge-hub udp_fw_upgrade library:
  FW_START 0x01 [size LE32][keyhash 32B]   -> [01][status][v2_chunk LE16]
  FW_DATA  0x02 [data<=511] (stop&wait)    -> [02][offset LE32]
  FW_END   0x03 [test u8][crc LE16]        -> [03][ok]
  REBOOT   0x05                            -> [05][01]

Usage:
  python tools/firmware_upgrade.py --ip 192.168.12.101 --src 192.168.12.150
         -f build/fw.signed.bin
"""
import argparse
import socket
import struct
import sys
import time

UDP_PORT = 8600
CHUNK = 511
START_TMO = 20.0
REPLY_TMO = 5.0


def crc16_ccitt(data):
    """Zephyr sys/crc.h crc16_ccitt(0, data) -- reflected, poly 0x1021."""
    seed = 0
    for b in data:
        seed = (seed >> 8 | seed << 8) & 0xFFFF
        seed ^= b
        seed ^= (seed & 0xFF) >> 4
        seed = (seed << 12) & 0xFFFF ^ seed
        seed = ((seed & 0xFF) << 5) & 0xFFFF ^ seed
    return seed


def keyhash_from_image(img):
    hdr_size = struct.unpack('<H', img[8:10])[0]
    img_size = struct.unpack('<I', img[12:16])[0]
    off = hdr_size + img_size
    magic, tlv_size = struct.unpack('<HH', img[off:off + 4])
    assert magic == 0x6907, 'not a signed MCUboot image'
    end = off + tlv_size
    off += 4
    while off + 4 <= end:
        tag, ln = struct.unpack('<HH', img[off:off + 4])
        if tag == 0x01:
            return img[off + 4:off + 4 + ln]
        off += 4 + (ln + 3) // 4 * 4
    raise SystemExit('image has no KEYHASH TLV')


class Udp:
    def __init__(self, src_ip, dst_ip):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind((src_ip, 0))
        self.dst = (dst_ip, UDP_PORT)

    def xfer(self, payload, tmo=REPLY_TMO):
        self.s.sendto(payload, self.dst)
        self.s.settimeout(tmo)
        r, _ = self.s.recvfrom(1024)
        return r

    def drain(self, secs):
        self.s.settimeout(secs)
        try:
            self.s.recvfrom(1024)
        except socket.timeout:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ip', required=True)
    ap.add_argument('--src', default='0.0.0.0')
    ap.add_argument('-f', '--file', required=True)
    ap.add_argument('--test', action='store_true')
    ap.add_argument('--no-reboot', action='store_true')
    ap.add_argument('--legacy', action='store_true',
                    help='force 0x02 stop-and-wait (old firmware without '
                         '0x06 handling advertises v2_chunk but drops V2)')
    args = ap.parse_args()

    img = open(args.file, 'rb').read()
    kh = keyhash_from_image(img)
    print('image %d bytes, keyhash %s' % (len(img), kh.hex()))

    u = Udp(args.src, args.ip)

    r = u.xfer(bytes([0x01]) + struct.pack('<I', len(img)) + kh,
               START_TMO)
    status, v2c = r[1], struct.unpack('<H', r[2:4])[0]
    print('FW_START -> status=%d v2_chunk=%d' % (status, v2c))
    if status != 1:
        sys.exit('start rejected')

    if v2c >= 512 and not args.legacy:
        off = send_v2(u, img, v2c)
    else:
        off = send_legacy(u, img)
    if off != len(img):
        sys.exit('transfer incomplete')

    crc = crc16_ccitt(img)
    r = u.xfer(bytes([0x03, 1 if args.test else 0]) +
               struct.pack('<H', crc))
    print('FW_END crc=0x%04x -> ok=%d' % (crc, r[1]))
    if r[1] != 1:
        sys.exit('end rejected (crc/keyhash verify failed)')

    if args.no_reboot:
        print('done (reboot skipped)')
        return
    u.drain(0.2)
    r = u.xfer(bytes([0x05]))
    print('REBOOT -> ok=%d, waiting for swap...' % r[1])

    # swap (SWAP_SCRATCH 搬 ~1.3MB 外部 NOR) + 新镜像启动 ~15-20s
    time.sleep(25)
    r = u.xfer(bytes([0x04]), 10)
    print('after reboot: %s' % r[1:].decode('ascii', 'replace'))


def send_legacy(u, img):
    """0x02 停等 (兼容老固件): 每块等应答。返回已确认字节数。"""
    off = 0
    t0 = time.time()
    while off < len(img):
        n = min(CHUNK, len(img) - off)
        r = u.xfer(bytes([0x02]) + img[off:off + n])
        got = struct.unpack('<I', r[1:5])[0]
        if got != off + n:
            sys.exit('data NAK at %d (device has %d)' % (off + n, got))
        off += n
        if off % (CHUNK * 100) == 0 or off == len(img):
            print('  %d/%d (%.0f KB/s)' % (off, len(img),
                  off / 1024 / max(time.time() - t0, 1e-3)))
    return off


V2_WINDOW = 8        # go-back-N 窗口帧数 (对齐 Zephyr UDP_FW_WINDOW)
V2_ACK_TMO = 1.0     # 窗口级 ACK 超时 (覆盖 NOR 渐进擦除的扇区擦停顿)
V2_MAX_RETRIES = 8   # 单窗口停滞重试上限


def send_v2(u, img, chunk):
    """0x06 窗口 go-back-N (对齐 Zephyr fw_data_v2_stream): 连发窗口帧,
    按回复的期望 offset 推进; 丢帧从最后确认处重传 (设备按 offset 去重)。"""
    total = len(img)
    off = 0
    retries = 0
    t0 = time.time()
    while off < total:
        win_end = min(off + V2_WINDOW * chunk, total)
        w = off
        while w < win_end:
            n = min(chunk, total - w)
            u.s.sendto(bytes([0x06]) + struct.pack('<I', w) +
                       img[w:w + n], u.dst)
            w += n

        deadline = time.time() + V2_ACK_TMO
        confirmed = off
        while confirmed < win_end:
            remain = deadline - time.time()
            if remain <= 0:
                break
            try:
                u.s.settimeout(remain)
                r, _ = u.s.recvfrom(64)
            except socket.timeout:
                break
            if len(r) >= 5 and r[0] == 0x06:
                roff = struct.unpack('<I', r[1:5])[0]
                if roff > confirmed:
                    confirmed = min(roff, total)
                    retries = 0
                    if confirmed % (chunk * 32) < chunk or confirmed == total:
                        print('  %d/%d (%.0f KB/s)' %
                              (confirmed, total,
                               confirmed / 1024 / max(time.time() - t0, 1e-3)))

        if confirmed >= win_end:
            off = confirmed
            continue
        retries += 1
        if retries > V2_MAX_RETRIES:
            sys.exit('V2 window stalled at %d (device stuck?)' % confirmed)
        off = confirmed
    return off


if __name__ == '__main__':
    main()
