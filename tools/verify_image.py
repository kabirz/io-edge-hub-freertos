#!/usr/bin/env python3
"""MCUboot 签名镜像离线校验 (模拟 boot 域 bootutil 的验证逻辑):
header 解析 -> SHA256 TLV -> RSA2048 签名 (对 PEM 公钥)。
用法: python3 tools/verify_image.py build-linux/fw.signed.bin \
          tools/keys/root-rsa2048.pem
烧写失败时先跑本工具: PASS = 镜像本身没问题, 问题在烧写侧 (截断/地址);
FAIL = 签名侧问题 (密钥不匹配 / imgtool 版本差异), 按提示处理。
"""
import hashlib
import sys

IMG_MAGIC = 0x96F3B83D
TLV_INFO_MAGIC = 0x6907
TLV_KEYHASH = 0x0001
TLV_SHA256 = 0x0010
TLV_RSA2048_PSS = 0x0020   # MCUboot 默认 RSA 方案
TLV_RSA2048_PKCS1 = 0x0023  # PKCS#1 v1.5 (本项目未用, 保险支持)
# EMSA-PKCS1-v1_5 SHA-256 DigestInfo
DI_SHA256 = bytes.fromhex('3031300d060960864801650304020105000420')


def mgf1(seed, length):
    out = b''
    for c in range((length + 31) // 32):
        out += hashlib.sha256(seed + c.to_bytes(4, 'big')).digest()
    return out[:length]


def rsa_pss_verify(sig, n, e, mhash):
    """EMSA-PSS (SHA-256, 盐长按编码推断, 同 mbedtls rsassa_pss_verify)"""
    k = (n.bit_length() + 7) // 8
    h_len = 32
    em = pow(int.from_bytes(sig, 'big'), e, n).to_bytes(k, 'big')
    if len(em) < h_len + 10:
        return False
    masked_db, h, tail = em[:k - h_len - 1], em[k - h_len - 1:-1], em[-1]
    if tail != 0xBC:
        return False
    db = bytes(a ^ b for a, b in
               zip(masked_db, mgf1(h, k - h_len - 1)))
    db = bytes([db[0] & 0x00]) + db[1:]  # emBits 顶部零位清掩码
    idx = db.find(b'\x01')
    if idx < 0:
        return False
    if any(db[:idx]):
        return False
    salt = db[idx + 1:]
    m = b'\x00' * 8 + mhash + salt
    return hashlib.sha256(m).digest() == h


# ---------- 极简 ASN.1 DER (只取 INTEGER, 供 RSA 公/私钥解析) ----------
def der_parse(buf, off=0):
    """返回 (tag, content_bytes, next_off)"""
    tag = buf[off]
    i = off + 1
    ln = buf[i]
    i += 1
    if ln & 0x80:
        n = ln & 0x7F
        ln = int.from_bytes(buf[i:i + n], 'big')
        i += n
    return tag, buf[i:i + ln], i + ln


def collect_ints(buf, ints):
    """递归收集 INTEGER (constructed 与 OCTET STRING 均下钻, 兼容
    PKCS#1 私钥 / PKCS#8 包裹 / SPKI 公钥)"""
    off = 0
    while off + 2 <= len(buf):
        try:
            tag, content, off = der_parse(buf, off)
        except Exception:
            return
        if tag == 0x02:
            ints.append(int.from_bytes(content, 'big'))
        elif tag & 0x20 or tag == 0x04:
            collect_ints(content, ints)


def pem_rsa_pub(path):
    """PEM (公钥或私钥) -> (n, e)。RSAPrivateKey/SPKI 中 e 紧跟 n 之后
    (e 通常只有 17 位, 不能按位宽过滤)"""
    b64 = []
    for line in open(path, encoding='ascii'):
        if '-----BEGIN' in line or '-----END' in line:
            continue
        b64.append(line.strip())
    import base64
    der = base64.b64decode(''.join(b64))
    ints = []
    collect_ints(der, ints)
    for i, v in enumerate(ints):
        if v.bit_length() > 512 and i + 1 < len(ints):
            return v, ints[i + 1]  # n, e
    raise SystemExit('pem 解析失败: 未找到 RSA 模数')


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    img = open(sys.argv[1], 'rb').read()
    n, e = pem_rsa_pub(sys.argv[2])

    # ---- header ----
    magic, hdr_size, img_size = struct_hdr(img)
    if magic != IMG_MAGIC:
        raise SystemExit('FAIL: 镜像 magic 不符 (%08x) -- 不是 MCUboot 镜像'
                         % magic)
    if hdr_size != 0x200:
        raise SystemExit('FAIL: hdr_size=%u != 0x200' % hdr_size)
    print('header: magic OK, hdr=0x%x img_size=%u total=%u'
          % (hdr_size, img_size, len(img)))
    if hdr_size + img_size + 4 > len(img):
        raise SystemExit('FAIL: TLV 区越界 -- 镜像被截断? (img_size=%u, '
                         '文件仅 %u B)' % (img_size, len(img)))

    # ---- TLV walk ----
    tlv_off = hdr_size + img_size
    tmagic, tsize = struct.unpack_from('<HH', img, tlv_off)
    if tmagic != TLV_INFO_MAGIC:
        raise SystemExit('FAIL: TLV magic 不符 (@%u = %04x) -- 镜像尾部'
                         '缺失/截断' % (tlv_off, tmagic))
    end = tlv_off + tsize
    tlv = {}
    off = tlv_off + 4
    while off + 4 <= end:
        tag, ln = struct.unpack_from('<HH', img, off)
        if tag == 0:
            break
        tlv.setdefault(tag, img[off + 4:off + 4 + ln])
        off += 4 + (ln + 3) & ~3
    print('TLV: %s' % ', '.join('%04x(%dB)' % (k, len(v))
                                for k, v in sorted(tlv.items())))

    # ---- SHA256 (bootutil 对 头(含填充)+payload 整段计算) ----
    calc = hashlib.sha256(img[:hdr_size + img_size]).digest()
    if TLV_SHA256 not in tlv:
        raise SystemExit('FAIL: 无 SHA256 TLV')
    if calc != tlv[TLV_SHA256]:
        raise SystemExit('FAIL: SHA256 不符 -- 镜像内容与签名时不同 (损坏?)')
    print('sha256: OK')

    # ---- keyhash (与编译期 fw_keyhash.h 同源的公钥指纹) ----
    if TLV_KEYHASH in tlv:
        # SPKI DER 的 SHA256: 从 (n,e) 重组开销大, 仅展示; boot 验证不用它
        print('keyhash TLV: %s (app 通道升级门禁用)' %
              tlv[TLV_KEYHASH].hex())
    else:
        print('keyhash TLV: 缺失 (仅影响升级通道预校验)')

    # ---- RSA 签名 ----
    sig = None
    scheme = None
    if TLV_RSA2048_PSS in tlv:
        sig, scheme = tlv[TLV_RSA2048_PSS], 'PSS'
    elif TLV_RSA2048_PKCS1 in tlv:
        sig, scheme = tlv[TLV_RSA2048_PKCS1], 'PKCS#1 v1.5'
    if sig is None:
        raise SystemExit('FAIL: 无 RSA2048 签名 TLV')
    if len(sig) != 256:
        raise SystemExit('FAIL: 签名长度 %u != 256' % len(sig))
    if scheme == 'PSS':
        ok = rsa_pss_verify(sig, n, e, calc)
    else:
        em = pow(int.from_bytes(sig, 'big'), e, n).to_bytes(256, 'big')
        expect = b'\x00\x01' + \
            b'\xff' * (256 - 3 - len(DI_SHA256) - 32) + \
            b'\x00' + DI_SHA256 + calc
        ok = em == expect
    if not ok:
        raise SystemExit('FAIL: RSA 签名验证不通过 -- 签名密钥与所给 PEM '
                         '不匹配! (检查是否用错了 tools/keys 下的 pem)')
    print('rsa2048 (%s): OK' % scheme)
    print('PASS: 镜像完整且签名有效 -- 若设备仍报 not valid,'
          '问题在烧写 (截断/地址), 建议改装 full.hex')


def struct_hdr(img):
    import struct
    magic = struct.unpack_from('<I', img, 0)[0]
    hdr_size = struct.unpack_from('<H', img, 8)[0]
    img_size = struct.unpack_from('<I', img, 12)[0]
    return magic, hdr_size, img_size


import struct  # noqa: E402

if __name__ == '__main__':
    main()
