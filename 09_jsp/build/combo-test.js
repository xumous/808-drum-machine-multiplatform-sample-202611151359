// 临时测试：比较单声部与多声部同时触发时，Java 512 块与 worklet 128 块的差异。
const fs = require('fs');
const workletPath = process.argv[2];
const basePath = process.argv[3];
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
const base = JSON.parse(fs.readFileSync(basePath, 'utf8').replace(/^\uFEFF/, ''));

function makeProject(pads) {
    const p = JSON.parse(JSON.stringify(base));
    p.patterns.forEach((pat) => pat.data.forEach((row) => row.forEach((st) => {
        st[0] = 0;
    })));
    p.patterns[0].steps = 16;
    p.pads.forEach((q, i) => {
        q.level = 0.75;
        q.pan = 0.5;
        q.mute = false;
        q.solo = false;
    });
    p.patterns[0].data[0][0] = [1, 118, 1, 100, 0, 1, 0];    // 与默认工程完全一致的 BD（含重音）
    p.patterns[0].data[15][0] = [1, 96, 0, 100, 0, 1, 0];    // 与默认工程完全一致的 CH
    Object.assign(p.master, {hpOn: false, lpOn: false, drive: 1, volume: 1, swing: 0, songMode: false});
    p.current = 0;
    return p;
}

function renderWorklet(project) {
    const proc = new P();
    proc.port.onmessage({data: {type: 'project', project}});
    proc.port.onmessage({data: {type: 'play'}});
    const L = new Float32Array(128), R = new Float32Array(128);
    const arr = new Float32Array(240000);
    let peak = 0, sum = 0;
    for (let b = 0; b < 1875; b++) {
        L.fill(0);
        R.fill(0);
        proc.process([], [[L, R]]);
        arr.set(L, b * 128);
        for (let i = 0; i < 128; i++) {
            const a = Math.abs(L[i]);
            if (a > peak) peak = a;
            sum += L[i] * L[i];
        }
    }
    return {arr, peak, rms: Math.sqrt(sum / 240000)};
}

function wavStats(buf) {
    const arr = new Float32Array(240000);
    let peak = 0, sum = 0;
    for (let i = 0; i < 240000; i++) {
        const v = buf.readInt16LE(44 + i * 4) / 32767;
        arr[i] = v;
        const a = Math.abs(v);
        if (a > peak) peak = a;
        sum += v * v;
    }
    return {arr, peak, rms: Math.sqrt(sum / 240000)};
}

(async () => {
    const out = [];
    for (const pads of [[0], [15], [0, 15]]) {
        const project = makeProject(pads);
        const js = renderWorklet(project);
        const res = await fetch(apiBase + '/api/export/wav?mode=pattern', {
            method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(project)
        });
        const java = wavStats(Buffer.from(await res.arrayBuffer()));
        let first = -1, maxd = 0;
        for (let i = 0; i < 240000; i++) {
            const d = Math.abs(js.arr[i] - java.arr[i]);
            if (d > 1e-4 && first < 0) first = i;
            if (d > maxd) maxd = d;
        }
        out.push({
            pads, javaPeak: +java.peak.toFixed(5), jsPeak: +js.peak.toFixed(5),
            javaRms: +java.rms.toFixed(5), jsRms: +js.rms.toFixed(5),
            java5632: +java.arr[5632].toFixed(5), js5632: +js.arr[5632].toFixed(5),
            firstDiff: first, maxDiff: +maxd.toFixed(5)
        });
    }
    console.log(JSON.stringify(out, null, 2));
})();
