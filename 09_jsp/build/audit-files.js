// 临时审计：读取 WAV/MIDI 文件头并计算峰值/RMS，供 VERIFY.txt 引用。
const fs = require('fs');
for (const f of process.argv.slice(2)) {
    const b = fs.readFileSync(f);
    const ascii = (o, n) => b.toString('ascii', o, o + n);
    if (f.toLowerCase().endsWith('.wav')) {
        let peak = 0, sum = 0, count = 0;
        for (let i = 44; i + 1 < b.length; i += 2) {
            const v = b.readInt16LE(i) / 32767;
            const a = Math.abs(v);
            if (a > peak) peak = a;
            sum += v * v;
            count++;
        }
        console.log(JSON.stringify({
            file: f.split('\\').pop(), bytes: b.length,
            riff: ascii(0, 4), wave: ascii(8, 4),
            sampleRate: b.readUInt32LE(24), channels: b.readUInt16LE(22), bits: b.readUInt16LE(34),
            frames: (b.length - 44) / 4,
            peak: +peak.toFixed(5), rms: +Math.sqrt(sum / Math.max(1, count)).toFixed(5)
        }));
    } else {
        console.log(JSON.stringify({
            file: f.split('\\').pop(), bytes: b.length,
            mthd: ascii(0, 4), format: b.readUInt16BE(8), tracks: b.readUInt16BE(10), ppq: b.readUInt16BE(12),
            mtrk: b.toString('ascii', 14, 18)
        }));
    }
}
