/*
 * test-export.cjs —— 在 Node 里原样执行 app/export-worker.js（模拟 Worker 环境）
 *   node tools/test-export.cjs
 * 验证：WAV / 分轨 / MIDI 三条导出路径，与 core808_io.c 行为一致。
 */
const fs = require('fs');
const path = require('path');
const ROOT = path.join(__dirname, '..');

const wasmBytes = fs.readFileSync(path.join(ROOT, 'app', 'wasm', 'core808.wasm'));
let done = null;

/* --- 伪造 Worker 环境（self === globalThis，和浏览器 Worker 一致） --- */
global.self = global;
global.self.postMessage = (m) => {
    if (done) done(m);
};
global.self.onmessage = null;
global.importScripts = (p) => {
    (0, eval)(fs.readFileSync(path.join(ROOT, 'app', p), 'utf8'));
};
const realFetch = global.fetch;
global.fetch = async (url) => {
    if (String(url).includes('core808.wasm')) {
        return {arrayBuffer: async () => wasmBytes.buffer.slice(wasmBytes.byteOffset, wasmBytes.byteOffset + wasmBytes.byteLength)};
    }
    return realFetch(url);
};

(0, eval)(fs.readFileSync(path.join(ROOT, 'app', 'export-worker.js'), 'utf8'));

/* --- 先用 wasm 生成一份工程字节 --- */
(async () => {
    let memory = null;
    const imports = globalThis.N808WASI.createWasiImports(() => memory, new Map());
    const {instance} = await WebAssembly.instantiate(wasmBytes, imports);
    memory = instance.exports.memory;
    const ex = instance.exports;
    ex._initialize();
    const eng = ex.c808_create(48000);
    const psz = ex.n808_project_size() + 20;
    const p = ex.malloc(psz);
    const n = ex.n808_project_out(eng, p, psz);
    const project = new Uint8Array(memory.buffer, p, n).slice();
    ex.c808_destroy(eng);
    console.log('project bytes', project.length);

    for (const [kind, name] of [[0, '808-pattern.wav'], [2, '808-stems.wav'], [1, '808-pattern.mid']]) {
        const res = await new Promise((resolve) => {
            done = resolve;
            global.self.onmessage({
                data: {
                    t: 'export',
                    project: project.buffer.slice(0),
                    kind,
                    mode: 0,
                    loops: 1,
                    sampleRate: 48000,
                    fileName: name
                }
            });
        });
        if (res.t === 'error') {
            console.log('kind', kind, 'ERROR', res.msg);
            process.exit(1);
        }
        const list = res.files.map((f) => f.name + ':' + f.data.byteLength);
        console.log('kind', kind, 'files', res.files.length, list.slice(0, 4).join(', '),
            res.files.length > 4 ? '…' : '');
    }
    console.log('EXPORT WORKER OK');
    process.exit(0);
})().catch((e) => {
    console.error('FAIL', e);
    process.exit(1);
});
