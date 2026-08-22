"""FTP 功能测试: 认证/传输模式/文件操作/路径防护/并发限制 (RFC 959)。"""
import ftplib
import io
import os
import re
import socket

import pytest

pytestmark = pytest.mark.functional

CREATED = []  # 测试创建的文件, 会话级清理


@pytest.fixture(scope="module", autouse=True)
def cleanup(dev, ftp_creds):
    yield
    user, password = ftp_creds
    try:
        f = dev.ftp(user, password)
    except Exception:
        return
    for name in CREATED:
        try:
            f.delete(name)
        except ftplib.error_perm:
            pass
    for name in ("t_dir", "d"):
        try:
            f.rmd(name)
        except ftplib.error_perm:
            pass
    f.quit()


def test_login_pwd_syst_feat(ftp):
    assert ftp.pwd() == "/"
    assert ftp.sendcmd("SYST") == "215 UNIX Type: L8"
    feat = ftp.sendcmd("FEAT")
    for want in ("SIZE", "PASV", "EPSV", "PORT", "EPRT", "REST STREAM",
                 "TYPE A;I", "NLST", "MKD", "RMD"):
        assert want in feat, (want, feat)


def test_stor_size_retr_roundtrip(dev, ftp):
    payload = os.urandom(8192)
    ftp.storbinary("STOR t_e2e.bin", io.BytesIO(payload))
    CREATED.append("t_e2e.bin")
    assert ftp.size("t_e2e.bin") == 8192
    buf = io.BytesIO()
    ftp.retrbinary("RETR t_e2e.bin", buf.write)
    assert buf.getvalue() == payload


def test_appe(ftp):
    ftp.storbinary("APPE t_e2e.bin", io.BytesIO(b"ABCD"))
    assert ftp.size("t_e2e.bin") == 8196


def test_ascii_mode(dev, ftp):
    ftp.voidcmd("TYPE A")
    ftp.storlines("STOR t_ascii.txt", io.BytesIO(b"line1\r\nline2\r\n"))
    CREATED.append("t_ascii.txt")
    lines = []
    ftp.retrlines("RETR t_ascii.txt", lines.append)
    assert lines == ["line1", "line2"], lines
    ftp.voidcmd("TYPE I")


def test_mkd_rmd_nlst_rename_delete(ftp):
    ftp.mkd("t_dir")
    listing = []
    ftp.retrlines("NLST", listing.append)
    assert "t_dir" in listing
    ftp.rename("t_e2e.bin", "t_renamed.bin")
    CREATED.remove("t_e2e.bin")
    CREATED.append("t_renamed.bin")
    assert ftp.size("t_renamed.bin") == 8196
    ftp.delete("t_renamed.bin")
    CREATED.remove("t_renamed.bin")
    ftp.rmd("t_dir")


def test_rest_resume_stor(ftp):
    payload = os.urandom(4096)
    ftp.storbinary("STOR t_resume.bin", io.BytesIO(payload[:2048]))
    CREATED.append("t_resume.bin")
    ftp.sendcmd("REST 2048")
    ftp.storbinary("STOR t_resume.bin", io.BytesIO(payload[2048:]))
    buf = io.BytesIO()
    ftp.retrbinary("RETR t_resume.bin", buf.write)
    assert buf.getvalue() == payload


def test_rest_retr_offset(ftp):
    payload = os.urandom(4096)
    ftp.storbinary("STOR t_resume.bin", io.BytesIO(payload))
    buf = io.BytesIO()
    ftp.retrbinary("RETR t_resume.bin", buf.write, rest=1024)
    assert buf.getvalue() == payload[1024:]


def test_epsv_manual(dev, ftp):
    resp = ftp.sendcmd("EPSV")
    m = re.search(r"\(\|\|\|(\d+)\|\)", resp)
    assert m, resp
    data = dev.tcp(int(m.group(1)))
    try:
        ftp.putcmd("RETR t_resume.bin")
        r = ftp.getresp()
        assert r.startswith("150"), r
        buf = b""
        while True:
            chunk = data.recv(4096)
            if not chunk:
                break
            buf += chunk
        r = ftp.getresp()
        assert r.startswith("226"), r
        assert len(buf) == 4096, len(buf)
    finally:
        data.close()


def test_port_active_mode(dev, ftp):
    assert dev.src, "PORT 模式需要可绑定的物理网卡源 IP (--src-ip)"
    ls = socket.socket()
    ls.bind((dev.src, 0))
    ls.listen(1)
    ls.settimeout(10)
    ip_parts = [int(x) for x in ls.getsockname()[0].split(".")]
    port = ls.getsockname()[1]
    try:
        ftp.sendcmd(f"PORT {','.join(map(str, ip_parts))},"
                    f"{port // 256},{port % 256}")
        ftp.putcmd("RETR t_resume.bin")
        r = ftp.getresp()
        assert r.startswith("150"), r
        conn, _ = ls.accept()
        buf = b""
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            buf += chunk
        conn.close()
        r = ftp.getresp()
        assert r.startswith("226"), r
        assert len(buf) == 4096, len(buf)
    finally:
        ls.close()


def test_path_traversal_neutralized(ftp):
    # norm_path 栈式防护: .. 不逃出根目录, 而是被钳制回根
    assert ftp.sendcmd("CWD ..").startswith("250")
    assert ftp.pwd() == "/"

    ftp.storbinary("STOR ../evil.bin", io.BytesIO(b"evil"))
    assert ftp.size("/evil.bin") == 4  # 落在根内, 未逃逸
    ftp.delete("evil.bin")

    with pytest.raises(ftplib.error_perm):
        ftp.retrbinary("RETR ../no_such_file", lambda b: None)
    with pytest.raises(ftplib.error_perm):
        ftp.sendcmd("SIZE ../no_such_file")
    with pytest.raises(ftplib.error_perm):
        ftp.sendcmd("DELE ../no_such_file")


def test_anonymous_readonly(dev, ftp):
    a = dev.ftp("anonymous", "x@y.z")
    try:
        a.retrlines("NLST", lambda line: None)  # 读允许
        for cmd, fn in (("STOR x", lambda: a.storbinary(cmd, io.BytesIO(b"x"))),
                        ("DELE x", lambda: a.delete("x")),
                        ("MKD x", lambda: a.mkd("x"))):
            with pytest.raises(ftplib.error_perm):
                fn()
    finally:
        a.quit()


def test_wrong_password_rejected(dev):
    with pytest.raises(ftplib.error_perm):
        dev.ftp("admin", "wrong-pass")


def test_unknown_command_502(ftp):
    with pytest.raises(ftplib.error_perm) as ei:
        ftp.sendcmd("ZZZZ")
    assert "502" in str(ei.value), ei.value


def test_fourth_client_rejected(dev, ftp, ftp_creds):
    user, password = ftp_creds
    c2 = dev.ftp(user, password)
    c3 = dev.ftp(user, password)
    try:
        c2.retrlines("NLST", lambda line: None)
        c3.retrlines("NLST", lambda line: None)
        with pytest.raises(ftplib.error_temp) as ei:  # 第 4 个: 421
            dev.ftp(user, password)
        assert "421" in str(ei.value), ei.value
    finally:
        c2.quit()
        c3.quit()
