#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对 ftpd.c 的端到端测试（跑的是同一份 WinCE 源码，只是套了 host 垫片）。

用法：
    python3 host/test_ftpd.py [可执行文件路径]

覆盖：登录、目录、上传/下载、断点、改名、删除、UTF-8 文件名、主动/被动模式、
EPSV、路径逃逸防护、只读模式、错误口令、未登录拒绝。
"""
import ftplib
import io
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
RUN = os.path.join(PROJ, ".run")
ROOT = os.path.join(RUN, "root")
HOME = os.path.join(RUN, "home")
# Android 上 /sdcard 是 noexec，可执行文件必须放到应用私有目录
BINDIR = os.environ.get("FTPD_TEST_BINDIR") or tempfile.mkdtemp(prefix="ftpd-bin-")
HOST = "127.0.0.1"
PORT = 2121
PORT_RO = 2122
PORT_GBK = 2123
PORT_LIM = 2124

RESULTS = []


def check(name, fn):
    try:
        fn()
        RESULTS.append((name, True, ""))
        print("ok   %s" % name)
    except Exception as e:  # noqa: BLE001
        RESULTS.append((name, False, repr(e)))
        print("FAIL %s -> %r" % (name, e))


def write_ini(port, ro=0, extra=""):
    os.makedirs(HOME, exist_ok=True)
    cfg = [
        ("Port", str(port)),
        ("Root", "\\"),
        ("User", "ftp"),
        ("Password", "ftp"),
        ("ReadOnly", str(ro)),
        ("MaxClients", "8"),
        ("LogFile", ""),
        ("PasvIP", ""),
        ("Encoding", "UTF8"),
    ]
    # extra 里的键覆盖默认值，避免 ini 出现重复键
    for line in extra.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            k, v = k.strip(), v.strip()
            for i, (ck, _) in enumerate(cfg):
                if ck.lower() == k.lower():
                    cfg[i] = (ck, v)
                    break
            else:
                cfg.append((k, v))
    with open(os.path.join(HOME, "ftpd.ini"), "w", encoding="utf-8") as f:
        for k, v in cfg:
            f.write("%s=%s\n" % (k, v))


def wait_port(port, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection((HOST, port), 0.3)
            s.close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def start_server(binary, port, ro=0, extra=""):
    write_ini(port, ro, extra)
    env = dict(os.environ)
    env["FTPD_SHIMROOT"] = ROOT
    env["FTPD_SHIMHOME"] = HOME
    proc = subprocess.Popen([binary], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    os.chmod(binary, 0o755)
    if not wait_port(port):
        out = b""
        try:
            out = proc.stdout.read(4000) if proc.stdout else b""
        except Exception:
            pass
        raise RuntimeError("server did not listen: %r" % out)
    return proc


def stop_server(proc):
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except Exception:
        proc.kill()


def connect(port=PORT, user="ftp", pw="ftp", pasv=True):
    f = ftplib.FTP()
    f.encoding = "utf-8"
    f.connect(HOST, port, timeout=10)
    f.set_pasv(pasv)
    f.login(user, pw)
    return f


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJ, ".build", "ftpd_host")
    binary = os.path.abspath(binary)
    if not os.path.exists(binary):
        print("missing binary: %s" % binary)
        return 2
    # 复制到可执行分区（/sdcard 在 Android 上是 noexec）
    exe = os.path.join(BINDIR, "ftpd_host")
    shutil.copy2(binary, exe)
    os.chmod(exe, 0o755)
    binary = exe

    shutil.rmtree(RUN, ignore_errors=True)
    os.makedirs(ROOT, exist_ok=True)
    os.makedirs(HOME, exist_ok=True)
    with open(os.path.join(ROOT, "hello.txt"), "wb") as f:
        f.write(b"hello wince\r\n")
    os.makedirs(os.path.join(ROOT, "sub"), exist_ok=True)

    srv = start_server(binary, PORT)
    ro_srv = None
    try:
        # ---------- 基本登录 / 目录 ----------
        def t_login():
            f = connect()
            assert f.pwd() == "/", f.pwd()
            f.quit()

        def t_no_login():
            s = socket.create_connection((HOST, PORT), 5)
            s.settimeout(5)
            buf = s.recv(200)
            assert buf.startswith(b"220"), buf
            s.sendall(b"PWD\r\n")
            buf = s.recv(200)
            assert buf.startswith(b"530"), buf
            s.close()

        def t_bad_password():
            try:
                connect(pw="wrong")
            except ftplib.error_perm as e:
                assert str(e).startswith("530"), e
                return
            raise AssertionError("bad password accepted")

        def t_cwd_mkdir_rmdir():
            f = connect()
            f.mkd("newdir")
            f.cwd("newdir")
            assert f.pwd() == "/newdir", f.pwd()
            f.cwd("..")
            assert f.pwd() == "/", f.pwd()
            f.rmd("newdir")
            f.quit()

        def t_rmd_nonempty():
            f = connect()
            f.cwd("sub")
            f.storbinary("STOR keep.txt", io.BytesIO(b"x"))
            f.cwd("/")
            try:
                f.rmd("sub")
            except ftplib.error_perm as e:
                assert str(e).startswith("550"), e
                f.quit()
                return
            raise AssertionError("removed non-empty dir")

        # ---------- 上传 / 下载 ----------
        def t_upload_download():
            f = connect()
            payload = b"0123456789" * 1000
            f.storbinary("STOR up.bin", io.BytesIO(payload))
            assert f.size("up.bin") == len(payload)
            out = bytearray()
            f.retrbinary("RETR up.bin", out.extend)
            assert bytes(out) == payload, "content mismatch"
            f.quit()

        def t_big_file():
            f = connect()
            payload = bytes(range(256)) * 4096  # 1 MiB
            f.storbinary("STOR big.bin", io.BytesIO(payload), blocksize=8192)
            out = bytearray()
            f.retrbinary("RETR big.bin", out.extend, blocksize=8192)
            assert bytes(out) == payload
            f.quit()

        def t_append():
            f = connect()
            f.storbinary("STOR app.txt", io.BytesIO(b"abc"))
            f.storbinary("APPE app.txt", io.BytesIO(b"def"))
            out = bytearray()
            f.retrbinary("RETR app.txt", out.extend)
            assert bytes(out) == b"abcdef", out
            f.quit()

        def t_resume():
            f = connect()
            payload = b"ABCDEFGHIJ" * 100
            f.storbinary("STOR res.bin", io.BytesIO(payload))
            f.voidcmd("TYPE I")
            f.sendcmd("REST 500")
            out = bytearray()
            f.retrbinary("RETR res.bin", out.extend)
            assert bytes(out) == payload[500:], (len(out), len(payload))
            f.quit()

        def t_rest_upload():
            f = connect()
            payload = bytes(range(200)) * 100
            f.storbinary("STOR rest_up.bin", io.BytesIO(payload))
            f.voidcmd("TYPE I")
            f.sendcmd("REST 5000")
            f.storbinary("STOR rest_up.bin", io.BytesIO(b"TAIL"))
            out = bytearray()
            f.retrbinary("RETR rest_up.bin", out.extend)
            assert bytes(out) == payload[:5000] + b"TAIL", len(out)
            f.delete("rest_up.bin")
            f.quit()

        def t_rest_beyond_eof():
            f = connect()
            f.storbinary("STOR short.bin", io.BytesIO(b"12345"))
            f.voidcmd("TYPE I")
            f.sendcmd("REST 999999")
            f.storbinary("STOR short.bin", io.BytesIO(b"XY"))
            out = bytearray()
            f.retrbinary("RETR short.bin", out.extend)
            # 不允许留下 1MB 的空洞文件
            assert bytes(out) == b"12345XY", bytes(out)
            f.delete("short.bin")
            f.quit()

        def t_mdtm_size():
            f = connect()
            r = f.sendcmd("MDTM hello.txt")
            assert re.match(r"^213 \d{14}$", r), r
            assert f.size("hello.txt") == len(b"hello wince\r\n")
            try:
                f.size("sub")
            except ftplib.error_perm as e:
                assert str(e).startswith("550"), e
                f.quit()
                return
            raise AssertionError("SIZE on directory succeeded")

        # ---------- 列表 ----------
        def t_list():
            f = connect()
            lines = []
            f.retrlines("LIST", lines.append)
            assert any(l.startswith("-") and "hello.txt" in l for l in lines), lines
            assert any(l.startswith("d") and " sub" in l for l in lines), lines
            names = f.nlst()
            assert "hello.txt" in names and "sub" in names, names
            f.quit()

        def t_mlsd():
            f = connect()
            entries = {name: facts for name, facts in f.mlsd()}
            assert "hello.txt" in entries and entries["hello.txt"]["type"] == "file", entries
            assert entries["sub"]["type"] == "dir", entries
            assert int(entries["hello.txt"]["size"]) == len(b"hello wince\r\n")
            assert re.match(r"^\d{14}$", entries["hello.txt"]["modify"])
            f.quit()

        def t_mlst():
            f = connect()
            r = f.sendcmd("MLST hello.txt")
            assert "type=file" in r and "hello.txt" in r, r
            f.quit()

        # ---------- 改名 / 删除 ----------
        def t_rename_delete():
            f = connect()
            f.storbinary("STOR r1.txt", io.BytesIO(b"x"))
            f.rename("r1.txt", "r2.txt")
            assert "r2.txt" in f.nlst()
            f.delete("r2.txt")
            assert "r2.txt" not in f.nlst()
            f.quit()

        # ---------- UTF-8 ----------
        def t_utf8():
            f = connect()
            name = "中文 文件.txt"
            f.storbinary("STOR " + name, io.BytesIO(b"utf8 name"))
            names = f.nlst()
            assert name in names, names
            out = bytearray()
            f.retrbinary("RETR " + name, out.extend)
            assert bytes(out) == b"utf8 name"
            assert f.size(name) == 9
            f.delete(name)
            f.quit()

        def t_utf8_dir():
            f = connect()
            name = "新建 文件夹"
            f.mkd(name)
            f.cwd(name)
            assert f.pwd() == "/" + name, f.pwd()
            f.cwd("/")
            f.rmd(name)
            f.quit()

        # ---------- 路径安全 ----------
        def t_traversal():
            f = connect()
            f.cwd("..")
            assert f.pwd() == "/", f.pwd()
            f.cwd("../..")
            assert f.pwd() == "/", f.pwd()
            f.storbinary("STOR ../escape.txt", io.BytesIO(b"inside"))
            assert os.path.exists(os.path.join(ROOT, "escape.txt")), "not inside root"
            assert not os.path.exists(os.path.join(RUN, "escape.txt")), "ESCAPED ROOT!"
            f.delete("/escape.txt")
            f.quit()

        def t_cwd_invalid():
            f = connect()
            try:
                f.cwd("no-such-dir")
            except ftplib.error_perm as e:
                assert str(e).startswith("550"), e
                f.quit()
                return
            raise AssertionError("cwd into missing dir succeeded")

        # ---------- 主动模式 ----------
        def t_active_mode():
            f = connect(pasv=False)
            payload = b"active mode payload" * 100
            f.storbinary("STOR act.bin", io.BytesIO(payload))
            out = bytearray()
            f.retrbinary("RETR act.bin", out.extend)
            assert bytes(out) == payload
            lines = []
            f.retrlines("LIST", lines.append)
            assert any("act.bin" in l for l in lines), lines
            f.delete("act.bin")
            f.quit()

        # ---------- EPSV ----------
        def t_epsv():
            s = socket.create_connection((HOST, PORT), 5)
            s.settimeout(8)
            fh = s.makefile("rwb")

            def rd():
                line = fh.readline()
                assert line, "connection closed"
                return line.decode("latin-1").strip()

            assert rd().startswith("220")
            fh.write(b"USER ftp\r\n"); fh.flush(); assert rd().startswith("331")
            fh.write(b"PASS ftp\r\n"); fh.flush(); assert rd().startswith("230")
            fh.write(b"EPSV\r\n"); fh.flush()
            r = rd()
            m = re.match(r"^229 .*\(\|\|\|(\d+)\|\)", r)
            assert m, r
            port = int(m.group(1))
            d = socket.create_connection((HOST, port), 5)
            fh.write(b"TYPE I\r\n"); fh.flush(); assert rd().startswith("200")
            fh.write(b"RETR hello.txt\r\n"); fh.flush(); assert rd().startswith("150")
            data = b""
            while True:
                chunk = d.recv(4096)
                if not chunk:
                    break
                data += chunk
            d.close()
            assert data == b"hello wince\r\n", data
            assert rd().startswith("226")
            fh.write(b"QUIT\r\n"); fh.flush(); rd()
            s.close()

        # ---------- 只读 ----------
        def t_readonly():
            nonlocal ro_srv
            ro_srv = start_server(binary, PORT_RO, ro=1)
            f = connect(port=PORT_RO)
            out = bytearray()
            f.retrbinary("RETR hello.txt", out.extend)
            assert bytes(out) == b"hello wince\r\n"
            attempts = [
                ("STOR", lambda: f.storbinary("STOR ro.txt", io.BytesIO(b"x"))),
                ("DELE", lambda: f.delete("hello.txt")),
                ("MKD", lambda: f.mkd("ro-dir")),
                ("RNFR", lambda: f.rename("hello.txt", "x.txt")),
                ("RMD", lambda: f.rmd("sub")),
            ]
            for cmd, fn in attempts:
                try:
                    fn()
                except ftplib.error_perm as e:
                    assert str(e).startswith("550"), (cmd, e)
                else:
                    raise AssertionError("%s allowed on read-only server" % cmd)
            f.quit()

        # ---------- 命令集 ----------
        def t_gbk_mode():
            nonlocal ro_srv
            gbk_srv = start_server(binary, PORT_GBK, ro=0, extra="Encoding=GBK\n")
            try:
                f = ftplib.FTP()
                f.encoding = "gbk"
                f.connect(HOST, PORT_GBK, timeout=10)
                f.set_pasv(True)
                f.login("ftp", "ftp")
                name = "中文文件.txt"
                f.storbinary("STOR " + name, io.BytesIO(b"gbk-name"))
                names = f.nlst()
                assert name in names, names
                assert name in os.listdir(ROOT), os.listdir(ROOT)
                out = bytearray()
                f.retrbinary("RETR " + name, out.extend)
                assert bytes(out) == b"gbk-name"
                f.delete(name)
                f.quit()
            finally:
                stop_server(gbk_srv)

        def t_max_clients():
            nonlocal ro_srv
            lim_srv = start_server(binary, PORT_LIM, ro=0, extra="MaxClients=2\n")
            try:
                held = []
                for _ in range(2):
                    c = socket.create_connection((HOST, PORT_LIM), 5)
                    c.settimeout(5)
                    assert c.recv(200).startswith(b"220")
                    held.append(c)
                extra = socket.create_connection((HOST, PORT_LIM), 5)
                extra.settimeout(5)
                resp = extra.recv(200)
                extra.close()
                assert resp.startswith(b"421"), resp
                for c in held:
                    c.close()
            finally:
                stop_server(lim_srv)

        def t_parallel_sessions():
            session = [connect() for _ in range(4)]
            for i, f in enumerate(session):
                f.storbinary("STOR par%d.bin" % i, io.BytesIO(b"p" * 4096))
            for i, f in enumerate(session):
                out = bytearray()
                f.retrbinary("RETR par%d.bin" % i, out.extend)
                assert bytes(out) == b"p" * 4096
                f.delete("par%d.bin" % i)
                f.quit()

        def t_misc():
            f = connect()
            assert f.sendcmd("SYST").startswith("215")
            feat = f.sendcmd("FEAT")
            assert "UTF8" in feat and "MLSD" in feat and "SIZE" in feat, feat
            assert f.sendcmd("NOOP").startswith("200")
            assert f.sendcmd("TYPE A").startswith("200")
            assert f.sendcmd("TYPE I").startswith("200")
            assert f.sendcmd("OPTS UTF8 ON").startswith("200")
            assert f.sendcmd("STAT").startswith("211")
            f.quit()

        tests = [
            ("login/pwd", t_login),
            ("reject before login", t_no_login),
            ("reject bad password", t_bad_password),
            ("cwd/mkd/rmdir", t_cwd_mkdir_rmdir),
            ("rmdir non-empty", t_rmd_nonempty),
            ("upload/download", t_upload_download),
            ("1 MiB file", t_big_file),
            ("append", t_append),
            ("rest resume", t_resume),
            ("rest upload resume", t_rest_upload),
            ("rest beyond eof", t_rest_beyond_eof),
            ("size/mdtm", t_mdtm_size),
            ("list/nlst", t_list),
            ("mlsd", t_mlsd),
            ("mlst", t_mlst),
            ("rename/delete", t_rename_delete),
            ("utf-8 filename", t_utf8),
            ("utf-8 dir", t_utf8_dir),
            ("path traversal", t_traversal),
            ("cwd invalid", t_cwd_invalid),
            ("active mode", t_active_mode),
            ("epsv", t_epsv),
            ("read-only", t_readonly),
            ("gbk encoding", t_gbk_mode),
            ("max clients", t_max_clients),
            ("parallel sessions", t_parallel_sessions),
            ("misc commands", t_misc),
        ]
        for name, fn in tests:
            check(name, fn)
    finally:
        stop_server(srv)
        if ro_srv:
            stop_server(ro_srv)

    failed = [r for r in RESULTS if not r[1]]
    print("\n%d/%d passed" % (len(RESULTS) - len(failed), len(RESULTS)))
    if failed:
        for name, _, err in failed:
            print("  FAIL %s: %s" % (name, err))
        print("\n--- server log ---")
        try:
            with open(os.path.join(HOME, "ftpd.log"), "r", encoding="utf-8", errors="replace") as f:
                print(f.read()[-6000:])
        except OSError:
            pass
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
