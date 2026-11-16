// 临时验证：逐个声部比较“服务端 Java 引擎导出的 WAV”与“worklet 实时合成”的峰值/RMS。
// 目的：证明浏览器端 16 个声部的公式与 02_java_swing 完全一致，而不是只听起来差不多。
const fs = require('fs');

const workletPath = process.argv[2];
const baseProjectPath = process.argv[3];
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
let ProcessorClass = null;
global.registerProcessor = (name, cls) => {
    ProcessorClass = cls;
};
eval(fs.readFileSync(workletPath, 'utf8'));

const base = JSON.parse(fs.readFileSync(baseProjectPath, 'utf8').replace(/^\uFEFF/, ''));

function wavStats(buf) {
    let peak = 0, sum = 0, count = 0;
    for (let i = 44; i + 1 < buf.length; i += 2) {
        const v = buf.readInt16LE(i) / 32767;
        const a = Math.abs(v);
        if (a > peak) peak = a;
        sum += v * v;
        count++;
    }
    return {peak, rms: Math.sqrt(sum / Math.max(1, count))};
}

function renderWorklet(project) {
    const proc = new ProcessorClass();
    proc.port.onmessage({data: {type: 'project', project}});
    proc.port.onmessage({data: {type: 'play'}});
    const L = new Float32Array(128), R = new Float32Array(128);
    let peak = 0, sum = 0, count = 0;
    for (let b = 0; b < 1875; b++) {
        L.fill(0);
        R.fill(0);
        proc.process([], [[L, R]]);
        for (let i = 0; i < 128; i++) {
            const a = Math.abs(L[i]);
            if (a > peak) peak = a;
            sum += L[i] * L[i];
            count++;
        }
    }
    return {peak, rms: Math.sqrt(sum / count)};
}

function makeProject(pad) {
    const p = JSON.parse(JSON.stringify(base));
    p.patterns.forEach((pat) => pat.data.forEach((row) => row.forEach((st) => {
        st[0] = 0;
    })));
    p.patterns[0].steps = 16;
    p.patterns[0].data[pad][0] = [1, 127, 0, 100, 0, 1, 0];
    p.pads.forEach((q, i) => {
        q.level = i === pad ? 0.75 : 0;
        q.pan = 0.5;
        q.mute = false;
        q.solo = false;
    });
    Object.assign(p.master, {hpOn: false, lpOn: false, drive: 1, volume: 1, swing: 0, songMode: false});
    p.current = 0;
    return p;
}

(async () => {
    const rows = [];
    for (let pad = 0; pad < 16; pad++) {
        const project = makeProject(pad);
        const js = renderWorklet(project);
        const res = await fetch(apiBase + '/api/export/wav?mode=pattern', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify(project)
        });
        if (!res.ok) throw new Error('HTTP ' + res.status + ' ' + await res.text());
        const buf = Buffer.from(await res.arrayBuffer());
        const java = wavStats(buf);
        rows.push({
            pad,
            javaPeak: Number(java.peak.toFixed(5)),
            workletPeak: Number(js.peak.toFixed(5)),
            peakDiff: Number((js.peak - java.peak).toFixed(5)),
            javaRms: Number(java.rms.toFixed(5)),
            workletRms: Number(js.rms.toFixed(5)),
            rmsDiff: Number((js.rms - java.rms).toFixed(5))
        });
    }
    console.log(JSON.stringify(rows, null, 2));
})();
