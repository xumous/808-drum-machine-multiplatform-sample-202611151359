#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把应用目录打包为单个自解压 EXE（无压缩归档 + C 启动器）。"""
import os
import struct
import subprocess
import sys
import tempfile
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MINGW = r"C:\Qt\Tools\mingw1310_64\bin"
LAUNCHER = os.path.join(ROOT, "pack808", "selfextract.c")
ICON = os.path.join(ROOT, "app.ico")


def build_payload(src, app, main_exe):
    files = []
    for base, dirs, names in os.walk(src):
        dirs.sort()
        names.sort()
        for n in names:
            full = os.path.join(base, n)
            rel = os.path.relpath(full, src).replace("\\", "/")
            files.append((rel, full))
    blobs = []
    table_len = 0
    for rel, full in files:
        with open(full, "rb") as f:
            data = f.read()
        name = rel.encode("utf-8")
        blobs.append((name, data))
        table_len += 2 + 4 + 8 + len(name)

    header = b"SX808001" + struct.pack("<I", len(files))
    header += app.encode("ascii")[:31].ljust(32, b"\0")
    header += main_exe.encode("ascii")[:63].ljust(64, b"\0")
    table = b""
    payload = b""
    offset = len(header) + table_len
    for name, data in blobs:
        table += struct.pack("<H", len(name)) + struct.pack("<I", len(data)) + struct.pack("<Q", offset)
        table += name
        payload += data
        offset += len(data)
    return header + table + payload


def pack(src, app, main_exe, out_exe):
    out_exe = os.path.abspath(out_exe)
    os.makedirs(os.path.dirname(out_exe), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="pack808_")
    try:
        payload = build_payload(src, app, main_exe)
        payload_path = os.path.join(tmp, "payload.bin")
        with open(payload_path, "wb") as f:
            f.write(payload)
        rc = os.path.join(tmp, "res.rc")
        icon_fwd = ICON.replace("\\", "/")
        with open(rc, "w", encoding="ascii") as f:
            f.write('1 RCDATA "payload.bin"\n')
            f.write('2 ICON "%s"\n' % icon_fwd)
        res_obj = os.path.join(tmp, "res.o")
        env = os.environ.copy()
        env["PATH"] = MINGW + os.pathsep + env.get("PATH", "")
        subprocess.run([os.path.join(MINGW, "windres.exe"), "-I", tmp, rc, "-O", "coff", "-o", res_obj],
                       check=True, cwd=tmp, env=env)
        cmd = [
            os.path.join(MINGW, "gcc.exe"), "-O2", "-std=c11", "-municode", "-mwindows",
            LAUNCHER, res_obj, "-o", out_exe,
            "-lole32", "-lshell32", "-luser32", "-static", "-s"
        ]
        subprocess.run(cmd, check=True, cwd=tmp, env=env)
        print("built", out_exe, os.path.getsize(out_exe), "bytes")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) < 5:
        print("usage: pack.py <srcDir> <appName> <mainExe> <outExe>")
        sys.exit(1)
    pack(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
