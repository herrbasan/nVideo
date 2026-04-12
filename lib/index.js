'use strict';

const path = require('path');

let nativeBinding = null;
let loadError = null;

try {
    const projectBinPath = path.join(__dirname, '..', 'bin', 'nvideo.node');
    nativeBinding = require(projectBinPath);
} catch (e) {
    loadError = e;
    try {
        const buildPath = path.join(__dirname, '..', 'build', 'Release', 'nvideo.node');
        nativeBinding = require(buildPath);
        loadError = null;
    } catch (e2) {
        try {
            const distPath = path.join(__dirname, '..', 'dist', 'nvideo.node');
            nativeBinding = require(distPath);
            loadError = null;
        } catch (e3) {
        }
    }
}

if (!nativeBinding) {
    throw new Error(
        'Failed to load nVideo native module.\n' +
        'Run "npm run build" to compile the native addon.\n' +
        'Original error: ' + (loadError ? loadError.message : 'module not found')
    );
}

module.exports = nativeBinding;
