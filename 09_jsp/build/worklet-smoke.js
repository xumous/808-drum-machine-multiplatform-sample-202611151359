// 临时 DSP 冒烟测试：在 Node 里伪造 AudioWorklet 全局，真正执行 worklet 的合成代码，
// 渲染 5 秒默认工程，检查是否有异常、是否产生非零音频，并与服务端 WAV 峰值对照。
// 徐某把它放 build 目录，只用于验证，不进入 WAR。
const fs = require('fs');

const workletPath = process.argv[2];
const projectPath = process.argv[3];

global.sampleRate = 48000;
const messages = [];
global.AudioWorkletProcessor = class {
    constructor() {
        this.port = {
            onmessage: null,
            postMessage: (m) => messages.push(m)
        };
    }
};
let ProcessorClass = null;
global.registerProcessor = (name, cls) => {
    ProcessorClass = cls;
};

const code = fs.readFileSync(workletPath, 'utf8');
eval(code);   // 在 Node 全局执行，等价于 AudioWorkletGlobalScope 加载模块

if (!ProcessorClass) {
    console.log('REGISTER_FAILED');
    process.exit(1);
}

const proc = new ProcessorClass();
const project = JSON.parse(fs.readFileSync(projectPath, 'utf8').replace(/^\uFEFF/, ''));
proc.port.onmessage({data: {type: 'project', project}});
proc.port.onmessage({data: {type: 'play'}});

const L = new Float32Array(128);
const R = new Float32Array(128);
const seconds = 5;
const blocks = Math.round(48000 * seconds / 128);
let peak = 0, sum = 0, nonzero = 0;

try {
    for (let b = 0; b < blocks; b++) {
        L.fill(0);
        R.fill(0);
        proc.process([], [[L, R]]);
        for (let i = 0; i < 128; i++) {
            const a = Math.abs(L[i]);
            if (a > peak) peak = a;
            sum += L[i] * L[i];
            if (a > 1e-6) nonzero++;
        }
    }
} catch (e) {
    console.log('PROCESS_EXCEPTION ' + (e && e.stack ? e.stack : e));
    process.exit(1);
}

const pos = messages.filter((m) => m.type === 'pos');
const ack = messages.filter((m) => m.type === 'ack');
console.log(JSON.stringify({
    registered: true,
    blocks,
    peak: Number(peak.toFixed(5)),
    rms: Number(Math.sqrt(sum / (blocks * 128)).toFixed(5)),
    nonzeroSamples: nonzero,
    posMessages: pos.length,
    lastPos: pos.length ? pos[pos.length - 1] : null,
    ack: ack.map((m) => m.action),
    errors: messages.filter((m) => m.type === 'error')
}, null, 2));
