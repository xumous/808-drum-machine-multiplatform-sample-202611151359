// 临时脚本：仅把 BD 第 0 步的重音关掉，用来确认“512 块 vs 128 块”的差异是否与重音有关。
const fs = require('fs');
const p = JSON.parse(fs.readFileSync(process.argv[2], 'utf8').replace(/^\uFEFF/, ''));
p.patterns[0].data[0][0][2] = 0;
fs.writeFileSync(process.argv[3], JSON.stringify(p));
console.log('accent off');
