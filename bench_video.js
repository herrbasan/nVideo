// Video transcoding benchmark (4K H.264 -> 4K AV1)
// Run: node bench_video.js

const nVideo = require('./lib/index.js');
const path = require('path');

const mp4File = 'D:/Work/_GIT/MediaService/tests/assets/videos/IMG_0104.MOV';
const outputFile = 'D:/Work/_GIT/nVideo/test_output_video_bench.mp4';

console.log('=== Video Transcode Benchmark: 4K HEVC -> H.264 ===\n');
console.log('Input:', mp4File);

let probeInfo;
try {
    probeInfo = nVideo.probe(mp4File);
    console.log('Probe result:');
    console.log('  Duration:', (probeInfo.duration / 1000000).toFixed(2), 'seconds');
    const video = probeInfo.streams.find(s => s.type === 'video');
    if (video) {
        console.log('  Video codec:', video.codec);
        console.log('  Resolution:', video.width, 'x', video.height);
        console.log('  FPS:', video.fps);
    }
    console.log();
} catch (e) {
    console.log('Probe error:', e.message);
    process.exit(1);
}

// Test 1: H.264 encode, single-threaded
console.log('--- Test 1: libx264, threads=1 ---');
const start1 = Date.now();
try {
    const result1 = nVideo.transcode(mp4File, outputFile.replace('.mp4', '_x264_st.mp4'), {
        video: { codec: 'libx264', crf: 23 },
        audio: {},  // Disable audio
        threads: 1
    });
    const elapsed1 = Date.now() - start1;
    const durationSec = probeInfo.duration / 1000000;
    console.log('Completed in:', elapsed1, 'ms');
    console.log('Speed:', (durationSec / (elapsed1 / 1000)).toFixed(2), 'x realtime');
    console.log('Output size:', result1?.size, 'bytes');
} catch (e) {
    console.log('Error:', e.message);
}
console.log();

// Test 2: H.264 encode, multi-threaded
console.log('--- Test 2: libx264, threads=4 ---');
const start2 = Date.now();
try {
    const result2 = nVideo.transcode(mp4File, outputFile.replace('.mp4', '_x264_mt.mp4'), {
        video: { codec: 'libx264', crf: 23 },
        audio: {},
        threads: 4
    });
    const elapsed2 = Date.now() - start2;
    const durationSec = probeInfo.duration / 1000000;
    console.log('Completed in:', elapsed2, 'ms');
    console.log('Speed:', (durationSec / (elapsed2 / 1000)).toFixed(2), 'x realtime');
    console.log('Output size:', result2?.size, 'bytes');
} catch (e) {
    console.log('Error:', e.message);
}
console.log();

// Test 3: Hardware acceleration (NVENC) - will fail gracefully if not available
console.log('--- Test 3: h264_nvenc (NVIDIA HW accel) ---');
const start3 = Date.now();
try {
    const result3 = nVideo.transcode(mp4File, outputFile.replace('.mp4', '_nvenc.mp4'), {
        video: { codec: 'h264_nvenc' },
        audio: {},
        threads: 1  // HW encoding doesn't benefit from more threads
    });
    const elapsed3 = Date.now() - start3;
    const durationSec = probeInfo.duration / 1000000;
    console.log('Completed in:', elapsed3, 'ms');
    console.log('Speed:', (durationSec / (elapsed3 / 1000)).toFixed(2), 'x realtime');
    console.log('Output size:', result3?.size, 'bytes');
} catch (e) {
    console.log('Error/Not available:', e.message);
}
console.log();

// Test 4: Stream copy (baseline)
console.log('--- Test 4: Stream copy (no re-encode) ---');
const start4 = Date.now();
try {
    const result4 = nVideo.remux(mp4File, outputFile.replace('.mp4', '_copy.mp4'), {});
    const elapsed4 = Date.now() - start4;
    const durationSec = probeInfo.duration / 1000000;
    console.log('Completed in:', elapsed4, 'ms');
    console.log('Speed:', (durationSec / (elapsed4 / 1000)).toFixed(2), 'x realtime');
    console.log('Output size:', result4?.size, 'bytes');
} catch (e) {
    console.log('Error:', e.message);
}
console.log();

console.log('=== Benchmark Complete ===');
console.log('\nNote: For AV1 HW encode test, try codec "libaom-av1" (CPU-only) or');
console.log('"av1_nvenc" if your NVIDIA driver supports it (40-series+).');