"""Modbus TCP client: MBAP framing, register/coil helpers."""
import struct


def adu(tid, unit, pdu):
    return struct.pack(">HHHB", tid, 0, len(pdu) + 1, unit) + pdu


class MbTcp:
    def __init__(self, dev, unit=1, timeout=None):
        self.dev = dev
        self.unit = unit
        self.tid = 0
        self.sock = dev.tcp(502, timeout)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _read_adu(self):
        buf = bytearray()
        while True:
            need = 6 if len(buf) < 6 else 6 + ((buf[4] << 8) | buf[5])
            if len(buf) >= need:
                return bytes(buf[:need])
            chunk = self.sock.recv(260)
            if not chunk:
                raise ConnectionError("modbus connection closed")
            buf += chunk

    def req(self, pdu, tid=None, proto=0, timeout=None):
        """Send one ADU, return the response PDU (tid/proto/unit validated)."""
        if tid is None:
            self.tid = (self.tid + 1) & 0xFFFF
            tid = self.tid
        self.sock.sendall(struct.pack(">HHHB", tid, proto, len(pdu) + 1,
                                      self.unit) + pdu)
        if timeout is not None:
            self.sock.settimeout(timeout)
        r = self._read_adu()
        rtid, rproto, rlen, runit = struct.unpack(">HHHB", r[:7])
        assert (rtid, rproto, runit) == (tid, proto, self.unit), r.hex()
        return r[7:6 + rlen]

    # request builders (PDU only)
    @staticmethod
    def read_req(fc, addr, count):
        return struct.pack(">BHH", fc, addr, count)

    @staticmethod
    def write_reg(addr, value):
        return struct.pack(">BHH", 6, addr, value)

    @staticmethod
    def write_coil(addr, on):
        return struct.pack(">BHBB", 5, addr, 0xFF if on else 0x00, 0x00)

    @staticmethod
    def write_regs(addr, values):
        return struct.pack(">BHHB", 16, addr, len(values),
                           len(values) * 2) + struct.pack(f">{len(values)}H",
                                                          *values)


def regs(pdu):
    """FC01-04 response PDU -> list of values (bits as 0/1, regs as ints)."""
    assert not pdu[0] & 0x80, f"modbus exception 0x{pdu[1]:02x}"
    if pdu[0] in (3, 4):
        return list(struct.unpack(f">{pdu[1] // 2}H", pdu[2:2 + pdu[1]]))
    bits = []
    for byte in pdu[2:2 + pdu[1]]:
        for i in range(8):
            bits.append((byte >> i) & 1)
    return bits


def exc_code(pdu):
    return pdu[1] if pdu[0] & 0x80 else None
