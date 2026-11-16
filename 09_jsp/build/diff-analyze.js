// 临时分析：把 worklet 输出与 Java 导出的 WAV 逐样本对比，定位差异是“相位/时间偏移”还是“幅度/公式”。
const fs = require('fs');
const workletPath = process.argv[2];
const projectPath = process.argv[3];
const apiBase = process.argv[4] || 'http://127.0.0.1:18080/xu808-jsp';

global.sampleRate = 48000;
global.AudioWorkletProcessor = class {
    constructor() {
        this.port = {
            onmessage: null, postMessage: () => {
            }
        };
    }
};
let P = null;
global.registerProcessor = (n, c) => {
    P = c;
};
eval(fs.readFileSync(workletPath, 'utf8'));

(async () => {
    const project = JSON.parse(fs.readFileSync(projectPath, 'utf8').replace(/^\uFEFF/, ''));
    const proc = new P();
    proc.port.onmessage({data: {type: 'project', project}});
    proc.port.onmessage({data: {type: 'play'}});
    const N = 240000;
    const js = new Float32Array(N);
    const L = new Float32Array(128), R = new Float32Array(128);
    for (let b = 0; b < N / 128; b++) {
        L.fill(0);
        R.fill(0);
        proc.process([], [[L, R]]);
        js.set(L, b * 128);
    }
    const res = await fetch(apiBase + '/api/export/wav?mode=pattern', {
        method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(project)
    });
    const buf = Buffer.from(await res.arrayBuffer());
    const java = new Float32Array(N);
    for (let i = 0; i < N; i++) java[i] = buf.readInt16LE(44 + i * 4) / 32767;

    function rmsDiff(lag) {
        let s = 0, n = 0;
        for (let i = 2000; i < N - 2000; i++) {
            const a = js[i];
            const b = java[i + lag];
            const d = a - b;
            s += d * d;
            n++;
        }
        return Math.sqrt(s / n);
    }

    const base = rmsDiff(0);
    const lags = {};
    for (let lag = -3; lag <= 3; lag++) lags[lag] = Number(rmsDiff(lag).toFixed(6));
    const blocks = [];
    for (let b = 0; b < 60; b++) {
        let s = 0;
        for (let i = b * 128; i < (b + 1) * 128; i++) {
            const d = js[i] - java[i];
            s += d * d;
        }
        blocks.push(Number(Math.sqrt(s / 128).toFixed(6)));
    }
    let firstDiff = -1;
    for (let i = 0; i < N; i++) if (Math.abs(js[i] - java[i]) > 1e-4) {
        firstDiff = i;
        break;
    }
    let maxDiff = 0, maxAt = -1;
    for (let i = 0; i < N; i++) {
        const d = Math.abs(js[i] - java[i]);
        if (d > maxDiff) {
            maxDiff = d;
            maxAt = i;
        }
    }
    console.log(JSON.stringify({
        rmsDiffLag0: Number(base.toFixed(6)),
        lags,
        blocks,
        firstDiff,
        js5628: Array.from(js.slice(5628, 5645)).map((v) => Number(v.toFixed(6))),
        java5628: Array.from(java.slice(5628, 5645)).map((v) => Number(v.toFixed(6))),
        maxDiff: Number(maxDiff.toFixed(6)),
        maxAt,
        jsAt: Number(js[maxAt].toFixed(6)),
        javaAt: Number(java[maxAt].toFixed(6)),
        jsAround: Array.from(js.slice(Math.max(0, maxAt - 3), maxAt + 4)).map((v) => Number(v.toFixed(5))),
        javaAround: Array.from(java.slice(Math.max(0, maxAt - 3), maxAt + 4)).map((v) => Number(v.toFixed(5)))
    }, null, 2));
})();
