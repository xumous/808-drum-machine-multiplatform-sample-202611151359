const fs = require('fs');
const b = fs.readFileSync('c:/Users/Happy/Pictures/watermark/guji808_202611142022/core808/test_out.wav');
// parse fmt chunk
let p = 12;
let channels = 2, bits = 24, dataOff = 0, dataLen = 0;
while (p < b.length - 8) {
    const id = b.toString('ascii', p, p + 4);
    const sz = b.readUInt32LE(p + 4);
    if (id === 'fmt ') {
        channels = b.readUInt16LE(p + 10);
        bits = b.readUInt16LE(p + 22);
    }
    if (id === 'data') {
        dataOff = p + 8;
        dataLen = sz;
        break;
    }
    p += 8 + sz + (sz & 1);
}
const bps = bits / 8;
const frames = Math.floor(dataLen / (bps * channels));
let sumL = 0, sumR = 0, peakL = 0, peakR = 0;
for (let i = 0; i < frames; i++) {
    const o = dataOff + i * bps * channels;
    const v = (a) => {
        let x = b[o + a] | (b[o + a + 1] << 8) | (b[o + a + 2] << 16);
        if (x & 0x800000) x -= 0x1000000;
        return x / 8388608;
    };
    const L = v(0), R = v(3);
    sumL += L * L;
    sumR += R * R;
    peakL = Math.max(peakL, Math.abs(L));
    peakR = Math.max(peakR, Math.abs(R));
}
console.log('channels', channels, 'bits', bits, 'frames', frames);
console.log('RMS L', Math.sqrt(sumL / frames).toFixed(5), 'R', Math.sqrt(sumR / frames).toFixed(5), 'peak L', peakL.toFixed(4), 'R', peakR.toFixed(4));
