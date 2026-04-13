// Simple audio transcoding test
// Run: node test_audio_simple.js

const nVideo = require('./lib/index.js');
const path = require('path');

const flacFile = 'D:/Work/_GIT/MediaService/tests/assets/audio/healme.flac';
const outputFile = 'D:/Work/_GIT/nVideo/test_output_simple.m4a';

console.log('=== Simple Audio Test: FLAC -> AAC ===\n');

// First, probe the file
const probeInfo = nVideo.probe(flacFile);
console.log('Probe result:');
console.log('  Duration (microseconds):', probeInfo.duration);
console.log('  Duration (seconds):', probeInfo.duration / 1000000);
console.log('  Audio codec:', probeInfo.streams.find(s => s.type === 'audio')?.codec);

const audioStream = probeInfo.streams.find(s => s.type === 'audio');
if (audioStream) {
    console.log('  Sample rate:', audioStream.sampleRate);
    console.log('  Channels:', audioStream.channels);
}

// Test with audio copy to see if output format is fine
console.log('\n--- Test: Stream copy (no re-encode) ---');
const start = Date.now();
try {
    const result = nVideo.remux(flacFile, outputFile.replace('.m4a', '_copy.m4a'), {});
    const elapsed = Date.now() - start;
    console.log('Stream copy completed in:', elapsed, 'ms');
    console.log('Result:', result);
} catch (e) {
    console.log('Error:', e.message);
}

console.log('\n--- Test: Transcode FLAC -> AAC ---');
const start2 = Date.now();
try {
    const result2 = nVideo.transcode(flacFile, outputFile, {
        video: {},  // Disable video
        audio: { codec: 'aac', bitrate: 128000 },
        threads: 1
    });
    const elapsed2 = Date.now() - start2;
    console.log('Transcode completed in:', elapsed2, 'ms');
    console.log('Result:', result2);
} catch (e) {
    console.log('Error:', e.message);
}

console.log('\n=== Done ===');