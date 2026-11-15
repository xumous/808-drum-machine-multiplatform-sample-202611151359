/*
 * engine.js —— 渲染进程侧的降级引擎（无 AudioWorklet 时使用）
 * ---------------------------------------------------------------------------
 * 主路径是 audio-worklet.js：DSP 在音频线程的 wasm 上跑，渲染进程只收 30fps
 * 快照。若 AudioWorklet / AudioContext 不可用，则退回这里：同一个 wasm 模块
 * 在渲染进程里以 25ms 定时器静默推进（没有声音，但编辑、导出、UI 全部可用）。
 */
(function (global) {
    'use strict';

    const SNAP_BYTES = 10856;
    const CMD_BYTES = 24;

    class LocalEngine {
        constructor(ex, sampleRate) {
            this.ex = ex;
            this.engine = 0;
            this.frames = 0;
            this.blocks = 0;
            this.sampleRate = sampleRate;
            this.snap = new Uint8Array(SNAP_BYTES);
            this.lPtr = ex.malloc(512);
            this.rPtr = ex.malloc(512);
            this.cmdPtr = ex.malloc(CMD_BYTES);
            this.snapPtr = ex.malloc(SNAP_BYTES);
            this.projPtr = ex.malloc(ex.n808_project_size() + 20);
            this.engine = ex.c808_create(sampleRate);
            this.refreshViews();
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

        send(op, a, b, c, n, f) {
            this.ex.n808_api(this.engine, op | 0, a | 0, b | 0, c | 0, n | 0, f || 0);
        }

        pushMidi(kind, a, b, c) {
            this.ex.c808_push_midi(this.engine, kind | 0, a | 0, b | 0, c | 0);
        }

        step(frames) {
            this.ex.c808_render(this.engine, this.lPtr, this.rPtr, frames);
            this.frames += frames;
            this.blocks++;
            this.refreshViews();
            this.ex.n808_pack_snapshot(this.engine, -1, this.frames, this.blocks, this.snapPtr);
            this.snap.set(this.u8.subarray(this.snapPtr, this.snapPtr + SNAP_BYTES));
        }

        projectBytes() {
            this.refreshViews();
            const n = this.ex.n808_project_out(this.engine, this.projPtr, this.ex.n808_project_size() + 20);
            if (n <= 0) return null;
            return this.u8.slice(this.projPtr, this.projPtr + n);
        }

        loadProject(bytes) {
            this.refreshViews();
            this.u8.set(bytes, this.projPtr);
            return this.ex.n808_project_in(this.engine, this.projPtr, bytes.length);
        }
    }

    async function createLocalEngine(sampleRate) {
        const bytes = await (await fetch('app://local/wasm/core808.wasm')).arrayBuffer();
        let memory = null;
        const imports = global.N808WASI.createWasiImports(() => memory, new Map());
        const {instance} = await WebAssembly.instantiate(bytes, imports);
        memory = instance.exports.memory;
        if (typeof instance.exports._initialize === 'function') instance.exports._initialize();
        return new LocalEngine(instance.exports, sampleRate);
    }

    global.N808Engine = {createLocalEngine, SNAP_BYTES};
})(window);
