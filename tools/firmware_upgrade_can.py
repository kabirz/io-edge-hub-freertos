"""CAN firmware upgrade client for the FreeRTOS io-edge-hub (PCAN-USB on
Windows / SocketCAN on Linux; protocol identical to apps/tools/
firmware_upgrade can mode).

Usage:
  python tools/firmware_upgrade_can.py version            # Windows (PCAN)
  python tools/firmware_upgrade_can.py version -c can0    # Linux SocketCAN
  python tools/firmware_upgrade_can.py upgrade -f build/test-v0.3.10.bin
  python tools/firmware_upgrade_can.py reboot

SocketCAN notes: bitrate is managed by ip link, not python-can. Bring the
interface up first, e.g.
  sudo ip link set can0 up type can bitrate 250000

Protocol (libs/can_fw_upgrade parity):
  0x101 [cmd LE32][arg LE32] / 0x102 reply / 0x103 data (8B/frame,
  ack every 512B) / 0x104 keyhash (5x [seq][7B]) / 0x105 version frags.
"""
import argparse
import re
import struct
import sys
import time

import can
from tqdm import tqdm

CAN_ID_CMD = 0x101
CAN_ID_REPLY = 0x102
CAN_ID_DATA = 0x103
CAN_ID_KEYHASH = 0x104
CAN_ID_VERSION = 0x105

FW_CMD_START, FW_CMD_CONFIRM, FW_CMD_VERSION, FW_CMD_REBOOT = 0, 1, 2, 3
FW_CODE_OFFSET = 0
FW_CODE_UPDATE_SUCCESS = 1
FW_CODE_CONFIRM = 3
FW_CODE_FLASH_ERROR = 4
FW_CODE_TRANSFER_ERROR = 5
FW_CODE_KEYHASH_ERROR = 6

CONFIRM_MAGIC = 0x55AA55AA
ACK_INTERVAL_FRAMES = 64  # 512B / 8B, 对齐 Zephyr CAN_FW_OFFSET_REPLY_BYTES


def resolve_iface(iface, channel):
    """auto: canN/vcanN -> Linux SocketCAN, 其余 (PCAN_USBBUS1...) -> PCAN."""
    if iface == 'auto':
        return 'socketcan' if re.fullmatch(r'v?can\d+', channel) else 'pcan'
    return iface


def check_socketcan(channel):
    """SocketCAN 码率由 ip link 管理; 未 UP 时给出可直接执行的命令."""
    try:
        state = open('/sys/class/net/%s/operstate' % channel).read().strip()
    except OSError:
        raise SystemExit('SocketCAN %s 不存在 (ip link 查看)' % channel)
    if state != 'up':
        raise SystemExit(
            'SocketCAN %s 处于 %s, 先执行:\n'
            '  sudo ip link set %s down type can bitrate <bitrate>\n'
            '  sudo ip link set %s up' % (channel, state, channel, channel))


class Dev:
    def __init__(self, iface, channel, bitrate):
        if iface == 'socketcan':
            check_socketcan(channel)
            self.bus = can.Bus(interface='socketcan', channel=channel)
        else:
            self.bus = can.Bus(interface='pcan', channel=channel,
                               bitrate=bitrate)

    def send(self, can_id, data):
        if len(data) > 8:
            raise SystemExit('CAN payload >8B')
        msg = can.Message(arbitration_id=can_id, data=data,
                          is_extended_id=False)
        for attempt in range(10):
            try:
                self.bus.send(msg, timeout=1.0)
                return
            except can.CanError as e:
                if 'No buffer space' in str(e) and attempt < 9:
                    time.sleep(0.01)
                    continue
                raise SystemExit('send 0x%03X failed: %s' % (can_id, e))

    def recv(self, can_id=None, timeout=5.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            msg = self.bus.recv(timeout=max(0.05, end - time.monotonic()))
            if msg is None:
                continue
            if msg.is_extended_id or msg.is_error_frame or msg.is_remote_frame:
                continue
            if can_id is not None and msg.arbitration_id != can_id:
                continue
            return bytes(msg.data)
        raise SystemExit('wait 0x%03X timeout (%.1fs)' % (can_id, timeout))

    def flush(self, dur=0.2):
        end = time.monotonic() + dur
        while time.monotonic() < end:
            if self.bus.recv(timeout=0.05) is None:
                break

    def fw_cmd(self, cmd, arg=0):
        self.send(CAN_ID_CMD, struct.pack('<II', cmd, arg))

    def fw_reply(self, timeout=5.0):
        d = self.recv(CAN_ID_REPLY, timeout)
        if len(d) < 8:
            raise SystemExit('0x102 reply DLC<8 (%dB)' % len(d))
        return struct.unpack('<II', d[:8])

    # ---- commands ----

    def get_version(self):
        self.flush()
        self.fw_cmd(FW_CMD_VERSION)
        code, total = self.fw_reply()
        if code != FW_CODE_OFFSET + 2 or not 0 < total <= 63:
            raise SystemExit('VERSION reply bad: code=%d len=%d' %
                             (code, total))
        buf = bytearray()
        for _ in range((total + 6) // 7):
            frag = self.recv(CAN_ID_VERSION)
            buf.extend(frag[1:8])
        nul = buf.find(0)
        return bytes(buf[:nul if nul >= 0 else total]).decode(
            'ascii', 'replace')

    def send_keyhash(self, keyhash):
        for seq in range(5):
            chunk = keyhash[seq * 7:(seq + 1) * 7]
            chunk = chunk + b'\0' * (7 - len(chunk))
            self.send(CAN_ID_KEYHASH, bytes([seq]) + chunk)
            time.sleep(0.005)

    def start(self, size):
        self.fw_cmd(FW_CMD_START, size)
        code, arg = self.fw_reply(timeout=10.0)  # erase window
        if code == FW_CODE_KEYHASH_ERROR:
            raise SystemExit('device rejected: keyhash mismatch')
        if code == FW_CODE_FLASH_ERROR:
            raise SystemExit('device rejected: flash error (arg=%d)' % arg)
        if code != FW_CODE_OFFSET or arg != 0:
            raise SystemExit('START unexpected: code=%d arg=%d' % (code, arg))

    def send_data(self, img, desc='upgrade'):
        off = 0
        n_in_block = 0
        with tqdm(total=len(img), unit='B', unit_scale=True,
                  unit_divisor=1024, desc=desc) as bar:
            while off + 8 <= len(img):
                self.send(CAN_ID_DATA, img[off:off + 8])
                off += 8
                bar.update(8)
                n_in_block += 1
                if n_in_block >= ACK_INTERVAL_FRAMES or off >= len(img):
                    code, arg = self.fw_reply()
                    if code == FW_CODE_UPDATE_SUCCESS:
                        return off
                    if code != FW_CODE_OFFSET:
                        raise SystemExit('data @%d: code=%d arg=%d' %
                                         (off, code, arg))
                    n_in_block = 0
            if off < len(img):
                self.send(CAN_ID_DATA, img[off:])
                bar.update(len(img) - off)
                off = len(img)
                code, arg = self.fw_reply()
                if code not in (FW_CODE_UPDATE_SUCCESS, FW_CODE_OFFSET):
                    raise SystemExit('tail data: code=%d arg=%d' % (off, code, arg))
        return off

    def confirm(self, permanent=True):
        self.fw_cmd(FW_CMD_CONFIRM, 1 if permanent else 0)
        code, arg = self.fw_reply()
        if code != FW_CODE_CONFIRM or arg != CONFIRM_MAGIC:
            raise SystemExit('CONFIRM bad: code=%d arg=0x%08X' % (code, arg))


def keyhash_from_image(path):
    """Parse MCUboot TLV KEYHASH (tag 0x0001) from a signed .bin."""
    img = open(path, 'rb').read()
    magic, hdr_size, img_size = struct.unpack_from('<I4xH2xI', img, 0)
    if magic != 0x96F3B83D:
        raise SystemExit('not a MCUboot image (magic=%08X)' % magic)
    tlv_off = hdr_size + img_size
    tlv_magic, tlv_size = struct.unpack_from('<HH', img, tlv_off)
    if tlv_magic != 0x6907:
        raise SystemExit('TLV magic bad at %d' % tlv_off)
    off = tlv_off + 4
    end = tlv_off + tlv_size
    while off + 4 <= end:
        tag, ln = struct.unpack_from('<HH', img, off)
        if tag == 0x0001 and ln == 32:
            return img[off + 4:off + 36]
        off += 4 + (ln + 3) & ~3
    raise SystemExit('no KEYHASH TLV found')


CAN_ID_BOOT_PROBE = 0x106
CAN_ID_BOOT_ACK = 0x107
BOOT_PROBE_MAGIC = 0x42544F31  # "BTO1"


def wait_boot_probe(dev, timeout=5.0):
    """等 MCUboot 0x106 探测帧 (应用 REBOOT 后), 校验后回 0x107."""
    t_end = time.monotonic() + timeout
    while time.monotonic() < t_end:
        try:
            d = dev.recv(CAN_ID_BOOT_PROBE, timeout=max(0.1, t_end - time.monotonic()))
        except SystemExit:
            continue
        if len(d) >= 8 and struct.unpack_from('<I', d, 0)[0] == BOOT_PROBE_MAGIC:
            print('bootloader probe: v%d.%d.%d' % (d[4], d[5], d[6]))
            dev.send(CAN_ID_BOOT_ACK, b'\x5a')
            return
        print('probe frame with bad magic:', d.hex())
    raise SystemExit('no boot probe 0x106 within %.1fs' % timeout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['version', 'upgrade', 'reboot',
                                    'bootupgrade', 'rescue'])
    ap.add_argument('-f', '--file')
    ap.add_argument('--iface', choices=['auto', 'socketcan', 'pcan'],
                    default='auto',
                    help='CAN backend (default: auto - canN->socketcan, '
                         'else pcan)')
    ap.add_argument('--channel', '-c', default='PCAN_USBBUS1',
                    help='PCAN channel or SocketCAN interface (can0)')
    ap.add_argument('--bitrate', type=int, default=250000,
                    help='PCAN only; socketcan bitrate set via ip link')
    ap.add_argument('--test', action='store_true',
                    help='temporary upgrade (revert on next boot)')
    args = ap.parse_args()

    dev = Dev(resolve_iface(args.iface, args.channel), args.channel,
              args.bitrate)
    try:
        if args.cmd == 'version':
            print('version:', dev.get_version())
            return
        if args.cmd == 'reboot':
            dev.fw_cmd(FW_CMD_REBOOT)
            print('reboot sent')
            return
        if args.cmd == 'rescue':
            # 砖机救援: 无有效镜像时 boot 持续发 0x106, 直接应答进入会话
            wait_boot_probe(dev, timeout=15.0)
            args.cmd = 'bootupgrade2'
            # fallthrough 共用升级流程 (不再发 REBOOT / 不再等探测)

        if args.cmd in ('bootupgrade', 'bootupgrade2'):
            img = open(args.file, 'rb').read()
            kh = keyhash_from_image(args.file)
            print('image %d bytes, keyhash %s' % (len(img), kh.hex()))
            if args.cmd == 'bootupgrade':
                dev.fw_cmd(FW_CMD_REBOOT)  # 让运行中的应用进 bootloader
                wait_boot_probe(dev)
            print('bootloader session open')
            dev.send_keyhash(kh)
            dev.start(len(img))
            print('slot0 erased, transferring...')
            dev.send_data(img, desc='slot0')
            dev.confirm(permanent=not args.test)
            print('confirmed; boot validates and starts new image')
            time.sleep(8)
            print('version:', dev.get_version())
            return

        img = open(args.file, 'rb').read()
        kh = keyhash_from_image(args.file)
        print('image %d bytes, keyhash %s' % (len(img), kh.hex()))
        dev.send_keyhash(kh)
        print('keyhash sent (5 frames)')
        dev.start(len(img))
        print('start ok, transferring...')
        dev.send_data(img)
        dev.confirm(permanent=not args.test)
        print('confirmed, rebooting for swap...')
        dev.fw_cmd(FW_CMD_REBOOT)
        time.sleep(25)
        print('after swap, version:', dev.get_version())
    finally:
        dev.bus.shutdown()


if __name__ == '__main__':
    main()
