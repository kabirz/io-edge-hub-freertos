import struct
import sys

from intelhex import IntelHex

f = sys.argv[1] if len(sys.argv) > 1 else 'build/full.hex'
ih = IntelHex(f)
print('segs:', [(hex(s), hex(e)) for s, e in ih.segments()])

d = bytes(ih.tobinarray(start=0x08010000, end=0x0801001f))
magic = struct.unpack('<I', d[:4])[0]
hdr_sz = struct.unpack('<H', d[8:10])[0]
img_sz = struct.unpack('<I', d[12:16])[0]
print('slot0 magic: 0x%08x (expect 0x96f3b83d)' % magic)
print('hdr_size: %d img_size: 0x%x' % (hdr_sz, img_sz))
ver = struct.unpack('<BBH', d[20:24])
print('version: %d.%d.%d' % ver)

v = bytes(ih.tobinarray(start=0x08010200, end=0x08010207))
print('vec: sp=0x%08x pc=0x%08x' % (struct.unpack('<I', v[:4])[0],
                                    struct.unpack('<I', v[4:])[0]))

# TLV: 位于 hdr_size + img_size (8 对齐)
tlv_off = (hdr_sz + img_sz + 7) & ~7
t = bytes(ih.tobinarray(start=0x08010000 + tlv_off,
                        end=0x08010000 + tlv_off + 7))
tmagic, tsz = struct.unpack('<HH', t[:4])
print('tlv: magic=0x%04x (expect 0x6907) size=%d @0x%x' %
      (tmagic, tsz, tlv_off))
p = tlv_off + 4
end = tlv_off + tsz
found = False
while p + 4 <= end and not found:
    ent = bytes(ih.tobinarray(start=0x08010000 + p,
                              end=0x08010000 + p + 4))
    tag, ln = struct.unpack('<HH', ent)
    if tag == 0x01:
        kh = bytes(ih.tobinarray(start=0x08010000 + p + 4,
                                 end=0x08010000 + p + 4 + ln))
        print('keyhash tag=0x01 len=%d: %s' % (ln, kh.hex()))
        found = True
    p += 4 + (ln + 3) // 4 * 4
