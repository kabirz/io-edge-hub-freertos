"""Modbus TCP probe: FC03 read holding regs, FC06 write DO reg."""
import socket
import struct
import sys
import time

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.12.101"
PORT = 502
UNIT = 1
SRC = sys.argv[2] if len(sys.argv) > 2 else None  # bind 直连绕过 TUN 代理


def adu(txid, pdu):
    return struct.pack(">HHHB", txid, 0, len(pdu) + 1, UNIT) + pdu


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    if SRC:
        s.bind((SRC, 0))  # 强制从物理网卡出, 避免被本机 TUN 代理截胡
    s.connect((IP, PORT))
    s.settimeout(3)

    # FC03: read holding 0x09..0x0D (slave id + 4 IP octets)
    pdu = bytes([3, 0x00, 0x09, 0x00, 0x05])
    s.sendall(adu(1, pdu))
    r = s.recv(260)
    print("FC03 reg 9..13 ->", r.hex())
    assert r[7] == 3 and r[8] == 10, "unexpected FC03 echo"

    # FC06: write DO reg (0x0000) = 0x0055 (DO1,3,5 on)
    pdu = bytes([6, 0x00, 0x00, 0x00, 0x55])
    s.sendall(adu(2, pdu))
    r = s.recv(260)
    print("FC06 reg 0 <- 0x55 ->", r.hex())
    assert r[7] == 6, "unexpected FC06 echo"

    # FC03 verify readback
    pdu = bytes([3, 0x00, 0x00, 0x00, 0x01])
    s.sendall(adu(3, pdu))
    r = s.recv(260)
    print("FC03 reg 0 ->", r.hex())

    # FC06 restore DO = 0
    pdu = bytes([6, 0x00, 0x00, 0x00, 0x00])
    s.sendall(adu(4, pdu))
    r = s.recv(260)
    print("FC06 reg 0 <- 0x00 ->", r.hex())

    # pipelined: two FC03 back-to-back in one packet; replies may coalesce
    s.sendall(adu(5, bytes([3, 0, 0, 0, 1])) + adu(6, bytes([3, 0, 1, 0, 1])))
    buf = b""
    replies = []
    s.settimeout(1.5)
    while len(replies) < 2:
        try:
            chunk = s.recv(260)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        while len(buf) >= 6:
            adu_len = 6 + ((buf[4] << 8) | buf[5])
            if len(buf) < adu_len:
                break
            replies.append(buf[:adu_len])
            buf = buf[adu_len:]
    for i, rep in enumerate(replies):
        print(f"pipeline {i + 1} ->", rep.hex())
    assert len(replies) == 2 and replies[0][0:2] == b"\x00\x05" \
        and replies[1][0:2] == b"\x00\x06", "pipeline replies wrong"

    s.close()
    print("MBTCP OK")


if __name__ == "__main__":
    main()
