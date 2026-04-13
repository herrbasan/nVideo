'use strict';

const nVideo = require('./lib');
const path = require('path');
const fs = require('fs');

const assetsDir = 'D:/Work/_GIT/MediaService/tests/assets';
const outputDir = 'D:/Work/_GIT/nVideo/test_media/test_output';

if (!fs.existsSync(outputDir)) {
    fs.mkdirSync(outputDir, { recursive: true });
}

function testVideoConcat() {
    console.log('\n=== Test: Video Concat ===\n');
    
    // Use two small video files for testing
    const file1 = path.join(assetsDir, 'videos/IMG_0104.MOV'); // 8.6s HEVC
    const file2 = path.join(assetsDir, 'videos/IMG_0104.MOV'); // Same file, concatenated
    const outputFile = path.join(outputDir, 'concat_test.mp4');
    
    // Clean up previous output
    if (fs.existsSync(outputFile)) {
        fs.unlinkSync(outputFile);
    }
    
    console.log('Input 1:', file1);
    console.log('Input 2:', file2);
    console.log('Output:', outputFile);
    
    // Probe inputs
    const info1 = nVideo.probe(file1);
    console.log('\nFile 1 duration:', info1.format.duration, 's');
    console.log('File 1 streams:', info1.streams.map(s => s.type + ' (' + s.codec + ')').join(', '));
    
    const expectedDuration = info1.format.duration * 2;
    console.log('Expected output duration:', expectedDuration, 's');
    
    // Concat
    let lastProgress = null;
    try {
        const result = nVideo.concat([file1, file2], outputFile, {
            onProgress: (p) => {
                lastProgress = p;
                process.stdout.write(`\rProgress: ${p.percent.toFixed(1)}% | ${p.time.toFixed(2)}s`);
            },
            onComplete: (r) => {
                console.log('\nComplete:', r);
            },
            onError: (e) => {
                console.error('\nError:', e);
            }
        });
        
        console.log('\n\nResult:', JSON.stringify(result, null, 2));
        
        // Verify output
        if (fs.existsSync(outputFile)) {
            const stat = fs.statSync(outputFile);
            console.log('Output file size:', (stat.size / 1024 / 1024).toFixed(2), 'MB');
            
            const outInfo = nVideo.probe(outputFile);
            console.log('Output duration:', outInfo.format.duration, 's');
            console.log('Output streams:', outInfo.streams.map(s => s.type + ' (' + s.codec + ')').join(', '));
            
            // Check if duration is approximately correct (within 10% tolerance)
            const durationDiff = Math.abs(outInfo.format.duration - expectedDuration);
            if (durationDiff < expectedDuration * 0.1) {
                console.log('\n✅ SUCCESS: Output duration is correct (', outInfo.format.duration, 's vs expected', expectedDuration, 's)');
            } else {
                console.log('\n❌ FAIL: Output duration mismatch (', outInfo.format.duration, 's vs expected', expectedDuration, 's)');
            }
        } else {
            console.log('\n❌ FAIL: Output file not created');
        }
    } catch (err) {
        console.error('\n❌ FAIL:', err.message);
        if (err.message.includes('non monotonically')) {
            console.error('Timestamp error detected!');
        }
    }
}

function testAudioConcat() {
    console.log('\n=== Test: Audio Concat (baseline) ===\n');
    
    const file1 = path.join(assetsDir, 'audio/healme.flac'); // 21.3s
    const file2 = path.join(assetsDir, 'audio/healme.flac');
    const outputFile = path.join(outputDir, 'concat_audio_test.flac');
    const listFile = outputFile + '.concat.txt';
    
    if (fs.existsSync(outputFile)) {
        fs.unlinkSync(outputFile);
    }
    
    console.log('Input 1:', file1);
    console.log('Input 2:', file2);
    
    const info1 = nVideo.probe(file1);
    console.log('File 1 duration:', info1.format.duration, 's');
    
    const expectedDuration = info1.format.duration * 2;
    console.log('Expected output duration:', expectedDuration, 's');
    
    try {
        const result = nVideo.concat([file1, file2], outputFile);
        console.log('Result:', JSON.stringify(result, null, 2));
        
        // Show the concat list file
        if (fs.existsSync(listFile)) {
            console.log('\nConcat list file:', fs.readFileSync(listFile, 'utf8'));
        }
        
        if (fs.existsSync(outputFile)) {
            const outInfo = nVideo.probe(outputFile);
            console.log('Output duration:', outInfo.format.duration, 's');
            
            const durationDiff = Math.abs(outInfo.format.duration - expectedDuration);
            if (durationDiff < expectedDuration * 0.1) {
                console.log('✅ SUCCESS: Audio concat duration correct');
            } else {
                console.log('❌ FAIL: Audio concat duration mismatch');
            }
        }
    } catch (err) {
        console.error('❌ FAIL:', err.message);
    }
}

// Run tests
console.log('nVideo Version:', nVideo.version);
testAudioConcat();
testVideoConcat();
