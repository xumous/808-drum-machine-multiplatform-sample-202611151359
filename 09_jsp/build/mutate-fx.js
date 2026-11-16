// 临时脚本：保留完整节奏，但关闭主效果，用来分离“声部求和”与“效果链”的差异。
const fs = require('fs');
const p = JSON.parse(fs.readFileSync(process.argv[2], 'utf8').replace(/^\uFEFF/, ''));
Object.assign(p.master, {hpOn: false, lpOn: false, drive: 1, volume: 1, swing: 0, songMode: false});
fs.writeFileSync(process.argv[3], JSON.stringify(p));
console.log('written ' + process.argv[3]);
