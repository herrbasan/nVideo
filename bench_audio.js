// Audio transcoding benchmark
// Run: node bench_audio.js

const nVideo = require('./lib/index.js');
const path = require('path');

const flacFile = 'D:/Work/_GIT/MediaService/tests/assets/audio/healme.flac';
const outputFile = 'D:/Work/_GIT/nVideo/test_output_audio_bench.m4a';

console.log('=== Audio Transcode Benchmark: FLAC -> AAC ===\n');
console.log('Input:', flacFile);

const probeInfo = nVideo.probe(flacFile);
console.log('Probe result:');
console.log('  Duration:', probeInfo.duration / 1000000, 'seconds');
console.log('  Audio codec:', probeInfo.streams.find(s => s.type === 'audio')?.codec);
console.log('  Sample rate:', probeInfo.streams.find(s => s.type === 'audio')?.sampleRate);
console.log('  Channels:', probeInfo.streams.find(s => s.type === 'audio')?.channels);
console.log();

// Test 1: Single-threaded
console.log('--- Test 1: Single-threaded (threads=1) ---');
const start1 = Date.now();
try {
    const result1 = nVideo.transcode(flacFile, outputFile.replace('.m4a', '_st.m4a'), {
        video: {},  // Disable video
        audio: { codec: 'aac', bitrate: 128000 },
        threads: 1
    });
    const elapsed1 = Date.now() - start1;
    console.log('Completed in:', elapsed1, 'ms');
    console.log('Speed:', (probeInfo.duration / 1000000 / (elapsed1 / 1000)).toFixed(2), 'x realtime');
    console.log('Output size:', result1?.size, 'bytes');
} catch (e) {
    console.log('Error:', e.message);
}
console.log();

// Test 2: Multi-threaded (4 threads)
console.log('--- Test 2: Multi-threaded (threads=4) ---');
const start2 = Date.now();
try {
    const result2 = nVideo.transcode(flacFile, outputFile.replace('.m4a', '_mt.m4a'), {
        video: {},
        audio: { codec: 'aac', bitrate: 128000 },
        threads: 4
    });
    const elapsed2 = Date.now() - start2;
    console.log('Completed in:', elapsed2, 'ms');
    console.log('Speed:', (probeInfo.duration / 1000000 / (elapsed2 / 1000)).toFixed(2), 'x realtime');
    console.log('Output size:', result2?.size, 'bytes');
} catch (e) {
    console.log('Error:', e.message);
}
console.log();

// Test 3: Stream copy (baseline - no re-encoding)
console.log('--- Test 3: Stream copy (no re-encode) ---');
const start3 = Date.now();
try {
    const result3 = nVideo.remux(flacFile, outputFile.replace('.m4a', '_copy.m4a'), {});
    const elapsed3 = Date.now() - start3;
    console.log('Completed in:', elapsed3, 'ms');
    console.log('Speed:', (probeInfo.duration / 1000000 / (elapsed3 / 1000)).toFixed(2), 'x realtime');
    console.log('Output size:', result3?.size, 'bytes');
} catch (e) {
    console.log('Error:', e.message);
}
console.log();

console.log('=== Benchmark Complete ===');