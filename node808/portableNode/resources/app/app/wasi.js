/*
 * wasi.js —— WASI preview1 导入实现（普通脚本，供 Worker / 渲染进程使用）
 * ---------------------------------------------------------------------------
 * AudioWorklet / 浏览器里没有文件系统，但 core808_io.c 用的是标准 stdio。
 * 这里把 fd_write / fd_seek / path_open 等映射到一个内存文件系统：
 *   - 写文件：字节留在内存里，导出完成后由主进程落盘；
 *   - 读文件：主进程先读进内存，再塞进 fs 表。
 * 不使用任何 Node API，纯 JS + WebAssembly.Memory。
 */
(function (global) {
    'use strict';

    const ERRNO_SUCCESS = 0;
    const ERRNO_BADF = 8;
    const ERRNO_NOENT = 44;
    const ERRNO_INVAL = 28;
    const OFLAGS_CREAT = 1;
    const OFLAGS_TRUNC = 8;

    function createWasiImports(getMemory, files) {
        const fs = files || new Map();          // path -> Uint8Array
        const fds = new Map();                  // fd -> { name, data, pos, writable }
        let nextFd = 4;
        let mem = null;
        let view = null;
        let bytes = null;

        function sync() {
            const m = getMemory();
            if (!m) return false;
            /* memory.grow() 会换掉 buffer（内存对象本身不变），必须按 buffer 判断 */
            if (!view || view.buffer !== m.buffer) {
                mem = m;
                view = new DataView(m.buffer);
                bytes = new Uint8Array(m.buffer);
            }
            return true;
        }

        function u8(ptr, len) {
            sync();
            return bytes.subarray(ptr, ptr + len);
        }

        function str(ptr, len) {
            sync();
            let s = '';
            for (let i = 0; i < len; i++) s += String.fromCharCode(bytes[ptr + i]);
            return s;
        }

        function putU32(ptr, v) {
            sync();
            view.setUint32(ptr, v >>> 0, true);
        }

        function putU64(ptr, v) {
            sync();
            view.setBigUint64(ptr, BigInt(v), true);
        }

        function getU32(ptr) {
            sync();
            return view.getUint32(ptr, true);
        }

        function newFile(name, data, writable, pos) {
            const fd = nextFd++;
            fds.set(fd, {name, data, pos: pos | 0, writable: !!writable});
            return fd;
        }

        function writeAt(entry, offset, src) {
            const need = offset + src.length;
            if (entry.data.length < need) {
                const grown = new Uint8Array(Math.max(need, Math.ceil(need * 1.5)));
                grown.set(entry.data);
                entry.data = grown;
            }
            entry.data.set(src, offset);
        }

        return {
            'wasi_snapshot_preview1': {
                args_get: () => ERRNO_SUCCESS,
                args_sizes_get: (argc, bufsize) => {
                    putU32(argc, 0);
                    putU32(bufsize, 0);
                    return 0;
                },
                environ_get: () => ERRNO_SUCCESS,
                environ_sizes_get: (count, bufsize) => {
                    putU32(count, 0);
                    putU32(bufsize, 0);
                    return 0;
                },
                clock_res_get: (_id, out) => {
                    putU64(Number(out), 1000n);
                    return 0;
                },
                clock_time_get: (_id, _prec, out) => {
                    const ns = BigInt(Math.round((global.performance ? global.performance.now() : Date.now()) * 1e6));
                    putU64(Number(out), ns);
                    return 0;
                },
                fd_write: (fd, iovs, iovsLen, nwritten) => {
                    sync();
                    const entry = fds.get(fd);
                    let total = 0;
                    for (let i = 0; i < iovsLen; i++) {
                        const base = iovs + i * 8;
                        const ptr = view.getUint32(base, true);
                        const len = view.getUint32(base + 4, true);
                        if (!len) continue;
                        if (entry && entry.writable) {
                            writeAt(entry, entry.pos + total, u8(ptr, len).slice());
                            total += len;
                        }
                    }
                    if (entry && entry.writable) entry.pos += total;
                    putU32(nwritten, total);
                    return 0;
                },
                fd_read: (fd, iovs, iovsLen, nread) => {
                    sync();
                    const entry = fds.get(fd);
                    let total = 0;
                    if (!entry) {
                        putU32(nread, 0);
                        return ERRNO_BADF;
                    }
                    for (let i = 0; i < iovsLen; i++) {
                        const base = iovs + i * 8;
                        const ptr = view.getUint32(base, true);
                        const len = view.getUint32(base + 4, true);
                        if (!len) continue;
                        const avail = Math.max(0, entry.data.length - entry.pos);
                        const n = Math.min(len, avail);
                        if (n > 0) {
                            bytes.set(entry.data.subarray(entry.pos, entry.pos + n), ptr);
                            entry.pos += n;
                            total += n;
                        }
                        if (n < len) break;
                    }
                    putU32(nread, total);
                    return 0;
                },
                fd_close: (fd) => {
                    const entry = fds.get(fd);
                    if (entry && entry.writable) fs.set(entry.name, entry.data);
                    fds.delete(fd);
                    return 0;
                },
                fd_seek: (fd, offset, whence, out) => {
                    const entry = fds.get(fd);
                    if (!entry) return ERRNO_BADF;
                    const off = Number(BigInt.asIntN(64, offset));
                    const w = Number(whence);
                    let pos = off;
                    if (w === 1) pos = entry.pos + off;
                    else if (w === 2) pos = entry.data.length + off;
                    if (pos < 0) pos = 0;
                    entry.pos = pos;
                    putU64(Number(out), BigInt(pos));
                    return 0;
                },
                fd_tell: (fd, out) => {
                    const entry = fds.get(fd);
                    if (!entry) return ERRNO_BADF;
                    putU64(Number(out), BigInt(entry.pos));
                    return 0;
                },
                fd_fdstat_get: (fd, out) => {
                    sync();
                    // fdstat: u8 filetype, u8 pad, u16 flags, u32 pad, u64 rights_base, u64 rights_inheriting
                    bytes.fill(0, out, out + 24);
                    if (fd === 0 || fd === 1 || fd === 2) {
                        bytes[out] = 2;               // character_device
                    } else if (fd === 3) {
                        bytes[out] = 3;               // directory
                    } else {
                        bytes[out] = 4;               // regular_file
                    }
                    view.setBigUint64(out + 8, 0xFFFFFFFFFFFFn, true);
                    view.setBigUint64(out + 16, 0xFFFFFFFFFFFFn, true);
                    return 0;
                },
                fd_fdstat_set_flags: () => 0,
                fd_filestat_get: (fd, out) => {
                    sync();
                    const entry = fds.get(fd);
                    bytes.fill(0, out, out + 64);
                    bytes[out + 16] = 4;
                    view.setBigUint64(out + 24, 1n, true);
                    view.setBigUint64(out + 32, BigInt(entry ? entry.data.length : 4096), true);
                    return 0;
                },
                fd_prestat_get: (fd, out) => {
                    sync();
                    if (fd !== 3) return ERRNO_BADF;
                    bytes.fill(0, out, out + 8);
                    bytes[out] = 0;                 // preopentype::dir
                    view.setUint32(out + 4, 1, true); // name len "/"
                    return 0;
                },
                fd_prestat_dir_name: (fd, path, len) => {
                    if (fd !== 3 || len < 1) return ERRNO_INVAL;
                    sync();
                    bytes[path] = 0x2f;
                    return 0;
                },
                path_open: (dirfd, _dirflags, path, pathLen, oflags, rights, _inherit, _fdflags, outFd) => {
                    const name = '/' + str(path, pathLen).replace(/^\/+/, '');
                    const flags = Number(oflags) >>> 0;
                    const writable = (BigInt.asUintN(64, rights) & 64n) !== 0n;
                    let data = fs.get(name);
                    if (!data) {
                        if (flags & OFLAGS_CREAT) data = new Uint8Array(0);
                        else return ERRNO_NOENT;
                    }
                    if (flags & OFLAGS_TRUNC) data = new Uint8Array(0);
                    putU32(outFd, newFile(name, data, writable, 0));
                    return 0;
                },
                path_filestat_get: () => 0,
                path_unlink_file: () => 0,
                path_create_directory: () => 0,
                path_remove_directory: () => 0,
                path_readlink: () => ERRNO_NOENT,
                sched_yield: () => 0,
                proc_exit: (code) => {
                    throw new Error('wasm proc_exit ' + code);
                },
                random_get: (ptr, len) => {
                    sync();
                    for (let i = 0; i < len; i++) bytes[ptr + i] = (Math.random() * 256) | 0;
                    return 0;
                },
                poll_oneoff: (_in, _out, _n, outN) => {
                    putU32(outN, 0);
                    return 0;
                }
            }
        };
    }

    global.N808WASI = {createWasiImports};
})(typeof self !== 'undefined' ? self : globalThis);
