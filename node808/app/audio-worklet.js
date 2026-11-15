/*
 * audio-worklet.js —— DSP 全部跑在 AudioWorklet 音频线程里的 wasm 实例上
 * ---------------------------------------------------------------------------
 *  - core808.c / core808_io.c / n808_glue.c 编译成 core808.wasm（无 JS DSP）
 *  - render() 内只做：调用 c808_render() + 把 2 个预分配的 float 缓冲区
 *    拷贝到输出通道；没有任何 JS 对象分配。
 *  - UI 命令通过 port.postMessage 进入，在块首由 worklet 转发给无锁命令队列。
 *  - 状态快照以 30fps 用“乒乓缓冲区”回传（transferable），主线程读完再传回，
 *    音频线程稳态零分配。
 */
const SNAP_BYTES = 10856;   /* sizeof(n808_snap)，见 src/n808_glue.c */

/* AudioWorkletGlobalScope 里没有 performance（Chromium 只在 Window/Worker 暴露），
   这里统一退化到 Date.now()；CPU 显示精度 1ms，够用且不影响音频路径。 */
const nowMs = (typeof performance !== 'undefined' && performance && performance.now)
    ? () => performance.now()
    : () => Date.now();

class Drum808Processor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.ready = false;
        this.ex = null;
        this.engine = 0;
        this.frames = 0;
        this.blocks = 0;
        this.cpuEma = 0;
        this.pool = [new Uint8Array(SNAP_BYTES), new Uint8Array(SNAP_BYTES)];
        this.snapPeriod = 0;
        this.nextSnap = 0;
        this.port.onmessage = (e) => this.onMessage(e.data);
    }

    /* WASI：worklet 里不做文件 I/O，全部返回“成功但无效果” */
    makeImports() {
        const m = () => this.ex.memory;
        const dv = () => new DataView(m().buffer);
        return {
            wasi_snapshot_preview1: {
                args_get: () => 0,
                args_sizes_get: (a, b) => {
                    const v = dv();
                    v.setUint32(a, 0, true);
                    v.setUint32(b, 0, true);
                    return 0;
                },
                environ_get: () => 0,
                environ_sizes_get: (a, b) => {
                    const v = dv();
                    v.setUint32(a, 0, true);
                    v.setUint32(b, 0, true);
                    return 0;
                },
                clock_res_get: (_id, out) => {
                    dv().setBigUint64(out, 1000n, true);
                    return 0;
                },
                clock_time_get: (_id, _p, out) => {
                    dv().setBigUint64(out, BigInt(Math.round(nowMs() * 1e6)), true);
                    return 0;
                },
                fd_write: (_fd, _iov, _n, written) => {
                    dv().setUint32(written, 0, true);
                    return 0;
                },
                fd_read: (_fd, _iov, _n, nread) => {
                    dv().setUint32(nread, 0, true);
                    return 0;
                },
                fd_close: () => 0,
                fd_seek: (_fd, _off, _wh, out) => {
                    dv().setBigUint64(out, 0n, true);
                    return 0;
                },
                fd_tell: (_fd, out) => {
                    dv().setBigUint64(out, 0n, true);
                    return 0;
                },
                fd_fdstat_get: (fd, out) => {
                    const mem = new Uint8Array(m().buffer);
                    mem.fill(0, out, out + 24);
                    mem[out] = fd < 3 ? 2 : 4;
                    dv().setBigUint64(out + 8, 0xFFFFFFFFFFFFn, true);
                    dv().setBigUint64(out + 16, 0xFFFFFFFFFFFFn, true);
                    return 0;
                },
                fd_fdstat_set_flags: () => 0,
                fd_filestat_get: (fd, out) => {
                    new Uint8Array(m().buffer).fill(0, out, out + 64);
                    return 0;
                },
                fd_prestat_get: () => 8,
                fd_prestat_dir_name: () => 8,
                path_open: () => 44,          /* ENOENT：worklet 内不需要文件 */
                path_filestat_get: () => 44,
                path_unlink_file: () => 44,
                path_create_directory: () => 44,
                path_remove_directory: () => 44,
                path_readlink: () => 44,
                sched_yield: () => 0,
                proc_exit: (code) => {
                    throw new Error('wasm proc_exit ' + code);
                },
                random_get: (ptr, len) => {
                    const mem = new Uint8Array(m().buffer);
                    for (let i = 0; i < len; i++) mem[ptr + i] = (Math.random() * 256) | 0;
                    return 0;
                },
                poll_oneoff: (_i, _o, _n, outN) => {
                    dv().setUint32(outN, 0, true);
                    return 0;
                }
            }
        };
    }

    async init(bytes, sampleRate) {
        const {instance} = await WebAssembly.instantiate(bytes, this.makeImports());
        const ex = instance.exports;
        this.ex = ex;
        if (typeof ex._initialize === 'function') ex._initialize();
        this.engine = ex.c808_create(sampleRate);
        if (!this.engine) {
            this.port.postMessage({t: 'error', msg: 'c808_create failed'});
            return;
        }
        this.lPtr = ex.malloc(512);
        this.rPtr = ex.malloc(512);
        this.cmdPtr = ex.malloc(24);
        this.snapPtr = ex.malloc(SNAP_BYTES);
        this.projPtr = ex.malloc(ex.n808_project_size() + 20);
        this.refreshViews();
        this.snapPeriod = Math.max(256, Math.round(sampleRate / 30));
        this.nextSnap = this.snapPeriod;
        this.ready = true;
        this.port.postMessage({t: 'ready', snapBytes: ex.n808_snap_size(), projectBytes: ex.n808_project_size()});
    }

    refreshViews() {
        const buf = this.ex.memory.buffer;
        if (this.memBuf === buf) return;
        this.memBuf = buf;
        this.f32 = new Float32Array(buf);
        this.i32 = new Int32Array(buf);
        this.u8 = new Uint8Array(buf);
        this.lOff = this.lPtr >> 2;
        this.rOff = this.rPtr >> 2;
        this.cmdOff = this.cmdPtr >> 2;
    }

    onMessage(msg) {
        if (!msg) return;
        switch (msg.t) {
            case 'init':
                this.init(msg.wasm, msg.sampleRate).catch((err) => {
                    this.port.postMessage({t: 'error', msg: String(err && err.message ? err.message : err)});
                });
                break;
            case 'cmd':
                if (!this.ready) return;
                this.ex.n808_api(this.engine, msg.op | 0, msg.a | 0, msg.b | 0, msg.c | 0,
                    msg.n | 0, msg.f || 0);
                break;
            case 'midi':
                if (!this.ready) return;
                this.ex.c808_push_midi(this.engine, msg.kind | 0, msg.a | 0, msg.b | 0, msg.c | 0);
                break;
            case 'getProject': {
                if (!this.ready) return;
                this.refreshViews();
                const n = this.ex.n808_project_out(this.engine, this.projPtr, this.ex.n808_project_size() + 20);
                if (n <= 0) {
                    this.port.postMessage({t: 'project', ok: false, err: n});
                    return;
                }
                const copy = this.u8.slice(this.projPtr, this.projPtr + n);
                this.port.postMessage({t: 'project', ok: true, buf: copy.buffer}, [copy.buffer]);
                break;
            }
            case 'loadProject': {
                if (!this.ready) return;
                this.refreshViews();
                const src = new Uint8Array(msg.buf);
                this.u8.set(src, this.projPtr);
                const rc = this.ex.n808_project_in(this.engine, this.projPtr, src.length);
                this.port.postMessage({t: 'projectLoaded', rc});
                break;
            }
            case 'getSong': {
                if (!this.ready) return;
                this.refreshViews();
                if (!this.songPtr) this.songPtr = this.ex.malloc(4 + 512 * 4);
                const n = this.ex.n808_song_out(this.engine, this.songPtr, 4 + 512 * 4);
                if (n <= 0) {
                    this.port.postMessage({t: 'song', buf: new ArrayBuffer(8)});
                    return;
                }
                const copy = this.u8.slice(this.songPtr, this.songPtr + n);
                this.port.postMessage({t: 'song', buf: copy.buffer}, [copy.buffer]);
                break;
            }
            case 'ret':
                this.pool.push(new Uint8Array(msg.buf));
                break;
            case 'ping':
                this.port.postMessage({t: 'pong', frames: this.frames});
                break;
            default:
                break;
        }
    }

    sendSnapshot() {
        if (!this.pool.length) return;          /* 上一帧还没被消费：丢弃，绝不阻塞音频 */
        const buf = this.pool.pop();
        this.refreshViews();
        this.ex.n808_pack_snapshot(this.engine, -1, this.frames, this.blocks, this.snapPtr);
        buf.set(this.u8.subarray(this.snapPtr, this.snapPtr + SNAP_BYTES));
        this.port.postMessage({t: 'snap', buf}, [buf.buffer]);
    }

    process(_inputs, outputs) {
        const out = outputs[0];
        if (!this.ready || !out || out.length === 0) return true;
        const L = out[0];
        const n = L.length;
        const t0 = nowMs();

        this.ex.c808_render(this.engine, this.lPtr, this.rPtr, n);
        this.refreshViews();

        L.set(this.f32.subarray(this.lOff, this.lOff + n));
        if (out.length > 1) out[1].set(this.f32.subarray(this.rOff, this.rOff + n));

        for (let i = 2; i < out.length; i++) out[i].fill(0);

        this.frames += n;
        this.blocks++;
        if (this.blocks === 1) this.port.postMessage({t: 'firstBlock', frames: n});

        /* Date.now() 精度只有 1ms，按 32 块（约 85ms）累计再算负载 */
        this.cpuAccMs = (this.cpuAccMs || 0) + (nowMs() - t0);
        this.cpuAccBlocks = (this.cpuAccBlocks || 0) + 1;
        if (this.cpuAccBlocks >= 32) {
            const elapsed = this.cpuAccMs / 1000;
            const wall = (this.cpuAccBlocks * n) / sampleRate;
            const load = wall > 0 ? elapsed / wall : 0;
            this.cpuEma = this.cpuEma * 0.6 + load * 0.4;
            this.ex.c808_report_cpu(this.engine, this.cpuEma);
            if (load > 1.0) this.ex.c808_report_xrun(this.engine);
            this.cpuAccMs = 0;
            this.cpuAccBlocks = 0;
        }

        if (this.frames >= this.nextSnap) {
            this.nextSnap = this.frames + this.snapPeriod;
            this.sendSnapshot();
        }
        return true;
    }
}

registerProcessor('drum808', Drum808Processor);
