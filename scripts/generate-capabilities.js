'use strict';

/**
 * Generate static capability JSON files from FFmpeg binary
 * Run this after building to create/update capability documentation
 *
 * Usage: node scripts/generate-capabilities.js [output-dir]
 * Default output: lib/capabilities/
 */

const fs = require('fs');
const path = require('path');

const OUTPUT_DIR = process.argv[2] || path.join(__dirname, '..', 'lib', 'capabilities');

// Load the native module
const nVideo = require(path.join(__dirname, '..', 'lib'));

function generateCapabilities() {
    console.log('Generating FFmpeg capability files...');
    console.log('=' .repeat(60));

    // Ensure output directory exists
    if (!fs.existsSync(OUTPUT_DIR)) {
        fs.mkdirSync(OUTPUT_DIR, { recursive: true });
        console.log('Created directory:', OUTPUT_DIR);
    }

    // 1. Build Info
    console.log('\n[1/5] Generating build info...');
    const buildInfo = nVideo.getBuildInfo();
    const buildInfoPath = path.join(OUTPUT_DIR, 'build-info.json');
    fs.writeFileSync(buildInfoPath, JSON.stringify(buildInfo, null, 2));
    console.log('  ->', buildInfoPath);
    console.log('      Version:', buildInfo.version);
    console.log('      HW Accels:', buildInfo.hwaccels.length);
    console.log('      Protocols:', buildInfo.protocols.length);

    // 2. All Codecs (grouped by type)
    console.log('\n[2/5] Generating codec info...');
    const allCodecs = nVideo.getCodecs();
    const videoCodecs = nVideo.getCodecs('video');
    const audioCodecs = nVideo.getCodecs('audio');
    const subtitleCodecs = nVideo.getCodecs('subtitle');

    const codecsPath = path.join(OUTPUT_DIR, 'codecs.json');
    fs.writeFileSync(codecsPath, JSON.stringify({
        all: allCodecs,
        byType: {
            video: videoCodecs,
            audio: audioCodecs,
            subtitle: subtitleCodecs
        },
        stats: {
            total: allCodecs.length,
            video: videoCodecs.length,
            audio: audioCodecs.length,
            subtitle: subtitleCodecs.length
        },
        generatedAt: new Date().toISOString()
    }, null, 2));
    console.log('  ->', codecsPath);
    console.log('      Total:', allCodecs.length);
    console.log('      Video:', videoCodecs.length);
    console.log('      Audio:', audioCodecs.length);
    console.log('      Subtitle:', subtitleCodecs.length);

    // 3. Common Codecs - organized by encoder/decoder for practical use
    console.log('\n[3/5] Generating common codecs list...');

    // Video encoders by category
    const commonEncoders = {
        // CPU encoders (best quality)
        cpu: [
            videoCodecs.find(c => c.name === 'libx264'),      // H.264 - industry standard
            videoCodecs.find(c => c.name === 'libx265'),      // H.265/HEVC - better compression
            videoCodecs.find(c => c.name === 'libsvtav1'),    // AV1 - SVT-AV1 (fastest)
            videoCodecs.find(c => c.name === 'libaom-av1'),   // AV1 - AOM (best quality)
            videoCodecs.find(c => c.name === 'librav1e'),     // AV1 - rav1e (Rust-based)
        ].filter(Boolean),
        // NVIDIA NVENC (fast, good for streaming/recording)
        nvidia: [
            videoCodecs.find(c => c.name === 'h264_nvenc'),
            videoCodecs.find(c => c.name === 'hevc_nvenc'),
            videoCodecs.find(c => c.name === 'av1_nvenc'),
        ].filter(Boolean),
        // Intel QSV (integrated GPU)
        intel: [
            videoCodecs.find(c => c.name === 'h264_qsv'),
            videoCodecs.find(c => c.name === 'hevc_qsv'),
            videoCodecs.find(c => c.name === 'av1_qsv'),
        ].filter(Boolean),
        // AMD AMF
        amd: [
            videoCodecs.find(c => c.name === 'h264_amf'),
            videoCodecs.find(c => c.name === 'hevc_amf'),
            videoCodecs.find(c => c.name === 'av1_amf'),
        ].filter(Boolean),
        // Other hardware
        other_hw: [
            videoCodecs.find(c => c.name === 'h264_vulkan'),
            videoCodecs.find(c => c.name === 'hevc_vulkan'),
            videoCodecs.find(c => c.name === 'av1_vulkan'),
        ].filter(Boolean),
        // Professional/lossless codecs
        professional: [
            videoCodecs.find(c => c.name === 'prores'),
            videoCodecs.find(c => c.name === 'dnxhd'),
            videoCodecs.find(c => c.name === 'ffv1'),
        ].filter(Boolean),
    };

    // Audio encoders (commonly used)
    const commonAudioEncoders = [
        audioCodecs.find(c => c.name === 'aac'),           // Most common
        audioCodecs.find(c => c.name === 'flac'),          // Lossless
        audioCodecs.find(c => c.name === 'libmp3lame'),    // MP3
        audioCodecs.find(c => c.name === 'libopus'),       // Best modern codec
        audioCodecs.find(c => c.name === 'ac3'),           // Dolby Digital
        audioCodecs.find(c => c.name === 'eac3'),          // Dolby Digital+
        audioCodecs.find(c => c.name === 'vorbis'),        // Ogg Vorbis
        audioCodecs.find(c => c.name === 'pcm_s16le'),     // Uncompressed PCM
    ].filter(Boolean);

    // Common decoders (what nVideo can read)
    const commonDecoders = {
        video: [
            videoCodecs.find(c => c.name === 'h264' && c.canDecode),
            videoCodecs.find(c => c.name === 'hevc' && c.canDecode),
            videoCodecs.find(c => c.name === 'av1' && c.canDecode),           // Generic AV1 decoder
            videoCodecs.find(c => c.name === 'libdav1d' && c.canDecode),      // Fastest AV1 decoder
            videoCodecs.find(c => c.name === 'vp9' && c.canDecode),
            videoCodecs.find(c => c.name === 'vp8' && c.canDecode),
            videoCodecs.find(c => c.name === 'mpeg4' && c.canDecode),
            videoCodecs.find(c => c.name === 'mpeg2video' && c.canDecode),
        ].filter(Boolean),
        audio: [
            audioCodecs.find(c => c.name === 'aac' && c.canDecode),
            audioCodecs.find(c => c.name === 'mp3' && c.canDecode),
            audioCodecs.find(c => c.name === 'flac' && c.canDecode),
            audioCodecs.find(c => c.name === 'opus' && c.canDecode),
            audioCodecs.find(c => c.name === 'vorbis' && c.canDecode),
            audioCodecs.find(c => c.name === 'ac3' && c.canDecode),
            audioCodecs.find(c => c.name === 'eac3' && c.canDecode),
            audioCodecs.find(c => c.name === 'pcm_s16le' && c.canDecode),
        ].filter(Boolean),
    };

    const commonCodecsPath = path.join(OUTPUT_DIR, 'codecs-common.json');
    fs.writeFileSync(commonCodecsPath, JSON.stringify({
        encoders: {
            video: commonEncoders,
            audio: commonAudioEncoders
        },
        decoders: commonDecoders,
        // Quick reference lists
        videoEncodersByHwaccel: {
            cpu: ['libx264', 'libx265', 'libsvtav1', 'libaom-av1'],
            nvidia: ['h264_nvenc', 'hevc_nvenc', 'av1_nvenc'],
            intel: ['h264_qsv', 'hevc_qsv', 'av1_qsv'],
            amd: ['h264_amf', 'hevc_amf', 'av1_amf'],
            vulkan: ['h264_vulkan', 'hevc_vulkan', 'av1_vulkan']
        },
        recommended: {
            webStreaming: { video: 'libx264', audio: 'aac' },
            archiving: { video: 'libx265', audio: 'flac' },
            modern: { video: 'libsvtav1', audio: 'libopus' },
            fastest: { video: 'h264_nvenc', audio: 'aac' }
        }
    }, null, 2));
    console.log('  ->', commonCodecsPath);
    console.log('      Video encoders by category');
    console.log('      Audio encoders:', commonAudioEncoders.length);
    console.log('      Decoders:', commonDecoders.video.length + commonDecoders.audio.length);

    // 4. All Filters (grouped by type)
    console.log('\n[4/5] Generating filter info...');
    const allFilters = nVideo.getFilters();
    const videoFilters = nVideo.getFilters('video');
    const audioFilters = nVideo.getFilters('audio');

    const filtersPath = path.join(OUTPUT_DIR, 'filters.json');
    fs.writeFileSync(filtersPath, JSON.stringify({
        all: allFilters,
        byType: {
            video: videoFilters,
            audio: audioFilters
        },
        stats: {
            total: allFilters.length,
            video: videoFilters.length,
            audio: audioFilters.length
        },
        generatedAt: new Date().toISOString()
    }, null, 2));
    console.log('  ->', filtersPath);
    console.log('      Total:', allFilters.length);
    console.log('      Video:', videoFilters.length);
    console.log('      Audio:', audioFilters.length);

    // 5. Formats
    console.log('\n[5/5] Generating format info...');
    const formats = nVideo.getFormats();
    const popularFormats = ['mov,mp4,m4a,3gp,3g2,mj2', 'matroska,webm', 'avi', 'mp3', 'flac', 'wav', 'ogg',
                           'hls', 'dash', 'rtmp', 'rtp'];

    const formatsPath = path.join(OUTPUT_DIR, 'formats.json');
    fs.writeFileSync(formatsPath, JSON.stringify({
        all: formats,
        popular: popularFormats.map(name => formats.find(f => f.name === name)).filter(Boolean),
        stats: {
            total: formats.length,
            canMux: formats.filter(f => f.canMux).length,
            canDemux: formats.filter(f => f.canDemux).length
        },
        generatedAt: new Date().toISOString()
    }, null, 2));
    console.log('  ->', formatsPath);
    console.log('      Total:', formats.length);

    // 6. Summary file (index)
    console.log('\n[6/5] Generating index...');
    const indexPath = path.join(OUTPUT_DIR, 'index.json');
    fs.writeFileSync(indexPath, JSON.stringify({
        ffmpeg: {
            version: buildInfo.version,
            configuration: buildInfo.configuration.substring(0, 100) + '...'
        },
        stats: {
            codecs: allCodecs.length,
            videoCodecs: videoCodecs.length,
            audioCodecs: audioCodecs.length,
            filters: allFilters.length,
            videoFilters: videoFilters.length,
            audioFilters: audioFilters.length,
            formats: formats.length,
            hwaccels: buildInfo.hwaccels.length,
            protocols: buildInfo.protocols.length
        },
        files: {
            buildInfo: 'build-info.json',
            codecs: 'codecs.json',
            commonCodecs: 'codecs-common.json',
            filters: 'filters.json',
            formats: 'formats.json'
        },
        generatedAt: new Date().toISOString()
    }, null, 2));
    console.log('  ->', indexPath);

    console.log('\n' + '='.repeat(60));
    console.log('Capability files generated successfully!');
    console.log('Output directory:', OUTPUT_DIR);
    console.log('\nYou can now import these files in your application:');
    console.log('  const codecs = require("nvideo/lib/capabilities/codecs.json");');
    console.log('  const filters = require("nvideo/lib/capabilities/filters.json");');
    console.log('  const formats = require("nvideo/lib/capabilities/formats.json");');
}

// Run generation
generateCapabilities();
