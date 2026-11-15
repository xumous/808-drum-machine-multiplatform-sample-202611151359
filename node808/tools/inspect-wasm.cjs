const fs = require('fs');
const buf = fs.readFileSync(process.argv[2]);
const m = new WebAssembly.Module(buf);
const groups = {};
for (const imp of WebAssembly.Module.imports(m)) {
    if (imp.kind !== 'function') continue;
    (groups[imp.module] = groups[imp.module] || []).push(imp.name);
}
for (const k of Object.keys(groups)) console.log('imports[' + k + ']: ' + groups[k].join(', '));
const ex = WebAssembly.Module.exports(m).map((e) => e.name + ':' + e.kind);
console.log('exports: ' + ex.join(', '));
