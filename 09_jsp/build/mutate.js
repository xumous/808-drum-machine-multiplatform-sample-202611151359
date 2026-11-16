// 临时脚本：把默认工程改成“只有底鼓第 0 步”的对照工程，用来逐样本比对 Java 与 worklet 的 DSP。
const fs = require('fs');
const p = JSON.parse(fs.readFileSync(process.argv[2], 'utf8').replace(/^\uFEFF/, ''));
p.patterns.forEach((pat) => {
    pat.data.forEach((row) => row.forEach((st) => {
        st[0] = 0;
    }));
});
p.patterns[0].steps = 16;
p.patterns[0].data[0][0] = [1, 127, 0, 100, 0, 1, 0];
p.pads.forEach((pad, i) => {
    pad.level = i === 0 ? 0.75 : 0;
    pad.pan = 0.5;
    pad.mute = false;
    pad.solo = false;
});
Object.assign(p.master, {hpOn: false, lpOn: false, drive: 1, volume: 1, swing: 0, songMode: false});
p.current = 0;
fs.writeFileSync(process.argv[3], JSON.stringify(p));
console.log('written ' + process.argv[3]);
