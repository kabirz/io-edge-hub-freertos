"""Extract IMG_TLV_KEYHASH from a signed MCUboot image (sanity check
against tools/gen_keyhash.py output)."""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
hdr_size = struct.unpack('<H', d[8:10])[0]
img_size = struct.unpack('<I', d[12:16])[0]
off = hdr_size + img_size
magic, tlv_size = struct.unpack('<HH', d[off:off + 4])
assert magic == 0x6907, hex(magic)
end = off + tlv_size
off += 4
while off + 4 <= end:
    tag, ln = struct.unpack('<HH', d[off:off + 4])
    if tag == 0x01:
        print('KEYHASH:', d[off + 4:off + 4 + ln].hex())
    off += 4 + (ln + 3) // 4 * 4
