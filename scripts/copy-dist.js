const fs = require('fs');
const path = require('path');

const MODULE_ROOT = path.join(__dirname, '..');
const BUILD_DIR = path.join(MODULE_ROOT, 'build', 'Release');
const DIST_DIR = path.join(MODULE_ROOT, 'dist');
const DEPS_BIN = path.join(MODULE_ROOT, 'deps', 'win', 'bin');

if (!fs.existsSync(DIST_DIR)) {
    fs.mkdirSync(DIST_DIR, { recursive: true });
}

const nodeFile = path.join(BUILD_DIR, 'nvideo.node');
if (!fs.existsSync(nodeFile)) {
    console.error('nvideo.node not found in build/Release/. Run npm run build first.');
    process.exit(1);
}

fs.copyFileSync(nodeFile, path.join(DIST_DIR, 'nvideo.node'));
console.log(`Copied: nvideo.node -> dist/`);

if (fs.existsSync(DEPS_BIN)) {
    const dlls = fs.readdirSync(DEPS_BIN).filter(f => f.endsWith('.dll'));
    for (const dll of dlls) {
        fs.copyFileSync(path.join(DEPS_BIN, dll), path.join(DIST_DIR, dll));
        console.log(`Copied: ${dll} -> dist/`);
    }
}

const capsDir = path.join(MODULE_ROOT, 'lib', 'capabilities');
if (fs.existsSync(capsDir)) {
    const capsDistDir = path.join(DIST_DIR, 'capabilities');
    if (!fs.existsSync(capsDistDir)) {
        fs.mkdirSync(capsDistDir, { recursive: true });
    }
    const caps = fs.readdirSync(capsDir).filter(f => f.endsWith('.json'));
    for (const cap of caps) {
        fs.copyFileSync(path.join(capsDir, cap), path.join(capsDistDir, cap));
    }
    if (caps.length > 0) console.log(`Copied: ${caps.length} capability files -> dist/capabilities/`);
}

console.log('\nDist copy complete.');
