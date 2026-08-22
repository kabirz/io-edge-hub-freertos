"""UDP firmware upgrade client (mirrors tools/firmware_upgrade.py, no tqdm).

Protocol (fw_udp.c / Zephyr udp_fw_upgrade):
  0x01 START [size LE32][keyhash 32B] -> [01][status][v2_chunk LE16]
  0x06 DATA_V2 [offset LE32][data<=chunk] -> [06][expected-offset LE32]
  0x03 END [test u8][crc LE16] -> [03][ok]
  0x05 REBOOT -> [05][01]
"""
import socket
import struct
import time

from helpers.device import UDP_CFG_PORT, parse_version

V2_WINDOW = 8
V2_ACK_TMO = 1.0
V2_MAX_RETRIES = 8


def crc16_ccitt(data):
    """Zephyr sys/crc.h crc16_ccitt(0, data) — reflected, poly 0x1021."""
    seed = 0
    for b in data:
        e = (seed ^ b) & 0xFF
        f = (e ^ (e << 4)) & 0xFF
        seed = ((seed >> 8) ^ (f << 8) ^ (f << 3) ^ (f >> 4)) & 0xFFFF
    return seed


def keyhash_from_image(img):
    hdr_size = struct.unpack("<H", img[8:10])[0]
    img_size = struct.unpack("<I", img[12:16])[0]
    off = hdr_size + img_size
    magic, tlv_size = struct.unpack("<HH", img[off:off + 4])
    assert magic == 0x6907, "not a signed MCUboot image"
    end = off + tlv_size
    off += 4
    while off + 4 <= end:
        tag, ln = struct.unpack("<HH", img[off:off + 4])
        if tag == 0x01:
            return img[off + 4:off + 4 + ln]
        off += 4 + (ln + 3) // 4 * 4
    raise ValueError("image has no KEYHASH TLV")


class FwUpg:
    def __init__(self, dev):
        self.dev = dev
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if dev.src:
            self.sock.bind((dev.src, 0))
        self.dst = (dev.ip, UDP_CFG_PORT)

    def close(self):
        self.sock.close()

    def xfer(self, payload, tmo=5.0):
        self.sock.sendto(payload, self.dst)
        self.sock.settimeout(tmo)
        r, _ = self.sock.recvfrom(1024)
        return r

    def start(self, img):
        r = self.xfer(bytes([0x01]) + struct.pack("<I", len(img))
                      + keyhash_from_image(img), 20.0)
        status = r[1]
        chunk = struct.unpack("<H", r[2:4])[0]
        assert status == 1, f"FW_START rejected: status={status}"
        return chunk

    def send_v2(self, img, chunk):
        total = len(img)
        off = 0
        retries = 0
        while off < total:
            win_end = min(off + V2_WINDOW * chunk, total)
            w = off
            while w < win_end:
                n = min(chunk, total - w)
                self.sock.sendto(bytes([0x06]) + struct.pack("<I", w)
                                 + img[w:w + n], self.dst)
                w += n
            deadline = time.time() + V2_ACK_TMO
            confirmed = off
            while confirmed < win_end and time.time() < deadline:
                try:
                    self.sock.settimeout(max(0.05, deadline - time.time()))
                    r, _ = self.sock.recvfrom(64)
                except socket.timeout:
                    break
                if len(r) >= 5 and r[0] == 0x06:
                    roff = struct.unpack("<I", r[1:5])[0]
                    confirmed = max(confirmed, min(roff, total))
            if confirmed >= win_end:
                off = confirmed
                retries = 0
                continue
            retries += 1
            assert retries <= V2_MAX_RETRIES, \
                f"V2 window stalled at {confirmed}/{total}"
            off = confirmed
        return off

    def end(self, img, test=False):
        r = self.xfer(bytes([0x03, 1 if test else 0])
                      + struct.pack("<H", crc16_ccitt(img)), 10.0)
        assert r[1] == 1, f"FW_END rejected (crc/keyhash): ok={r[1]}"

    def reboot(self):
        try:
            r = self.xfer(bytes([0x05]), 3.0)
        except socket.timeout:
            return  # device may already be rebooting (e.g. after END)
        # Zephyr replies 1 byte, the FreeRTOS port appends the ok flag
        assert r[0] == 0x05 and r[1:2] in (b"", b"\x01"), r.hex()


def wait_online(dev, timeout, probe_interval=1.0):
    """Poll UDP GET_VERSION until the device answers again after reboot."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = parse_version(dev.udp_xfer(b"\x04", timeout=2.0))
            return last
        except (socket.timeout, OSError, AssertionError):
            time.sleep(probe_interval)
    raise TimeoutError(f"device not back online within {timeout}s (last={last})")
