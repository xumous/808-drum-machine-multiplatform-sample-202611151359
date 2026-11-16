const fs = require('fs');
const s = fs.readFileSync(process.argv[2], 'utf8');
const m = s.match(/window\.XU808_INIT = ([\s\S]*?);\n/);
if (!m) {
    console.log('INIT_NOT_FOUND');
    process.exit(1);
}
const j = JSON.parse(m[1]);
console.log('INIT_OK patterns=' + j.patterns.length + ' pads=' + j.pads.length + ' current=' + j.current + ' steps=' + j.patterns[0].steps + ' rows=' + j.patterns[0].data.length + ' cols=' + j.patterns[0].data[0].length);
console.log('BASE=' + (s.match(/window\.XU808_BASE = '([^']*)'/) || [])[1]);
console.log('HAS_WORKLET=' + s.includes('xu808-worklet.js'));
console.log('CELL_MARKUP=' + s.includes('class="trackRow"'));
console.log('CHINESE_OK=' + s.includes('底鼓') + ' 徐某=' + s.includes('徐某'));
