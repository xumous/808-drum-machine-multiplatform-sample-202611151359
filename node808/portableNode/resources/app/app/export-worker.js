/*
 * export-worker.js —— 离线导出（WAV / 分轨 / MIDI）
 * ---------------------------------------------------------------------------
 * 导出走的是 core808_io.c 的原生实现：新建独立引擎 + 复制工程 + 同样的
 * c808_render() 路径。这里只提供一个内存文件系统（wasi.js），渲染完成后
 * 把文件字节回传给渲染进程，由主进程负责落盘。
 */
importScripts('wasi.js');

function cstr(ex, mem, str) {
    const bytes = new Uint8Array(str.length + 1);
    for (let i = 0; i < str.length; i++) bytes[i] = str.charCodeAt(i) & 0xff;
    const ptr = ex.malloc(bytes.length);
    new Uint8Array(mem.buffer).set(bytes, ptr);
    return ptr;
}

async function doExport(msg) {
    const files = new Map();
    let memory = null;
    const bytes = await (await fetch('app://local/wasm/core808.wasm')).arrayBuffer();
    const imports = N808WASI.createWasiImports(() => memory, files);
    const {instance} = await WebAssembly.instantiate(bytes, imports);
    const ex = instance.exports;
    memory = ex.memory;
    if (typeof ex._initialize === 'function') ex._initialize();

    const sr = msg.sampleRate || 48000;
    const eng = ex.c808_create(sr);
    if (!eng) throw new Error('c808_create failed');

    const projPtr = ex.malloc(msg.project.byteLength);
    new Uint8Array(memory.buffer).set(new Uint8Array(msg.project), projPtr);
    const rcLoad = ex.n808_project_in(eng, projPtr, msg.project.byteLength);
    if (rcLoad !== 0) throw new Error('工程数据无效 (rc=' + rcLoad + ')');

    /* 让 load / songOn 命令在块首生效 */
    const l = ex.malloc(1024);
    const r = ex.malloc(1024);
    ex.c808_set_song_on(eng, msg.mode === 1 ? 1 : 0);
    ex.c808_render(eng, l, r, 256);
    ex.c808_panic(eng);
    ex.c808_render(eng, l, r, 256);

    const path = cstr(ex, memory, '/' + msg.fileName);
    let rc;
    if (msg.kind === 1) {
        rc = ex.c808_export_midi(eng, path, msg.mode, msg.loops | 0);
    } else {
        rc = ex.c808_export_wav(eng, path, msg.mode, msg.loops | 0, 2, msg.kind === 2 ? 1 : 0);
    }
    ex.c808_destroy(eng);
    if (rc !== 0) throw new Error('导出失败 (rc=' + rc + ')');

    const out = [];
    const transfers = [];
    for (const [name, data] of files) {
        const copy = data.slice();
        out.push({name: name.replace(/^\//, ''), data: copy.buffer});
        transfers.push(copy.buffer);
    }
    if (!out.length) throw new Error('导出未生成文件');
    self.postMessage({t: 'done', files: out}, transfers);
}

self.onmessage = (e) => {
    const msg = e.data;
    if (!msg || msg.t !== 'export') return;
    doExport(msg).catch((err) => {
        self.postMessage({t: 'error', msg: String(err && err.message ? err.message : err)});
    });
};
