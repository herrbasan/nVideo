'use strict';

const path = require('path');
const fs = require('fs');
const crypto = require('crypto');

let nativeBinding = null;
let loadError = null;

// ==================== Caching System ====================

const DEFAULT_CACHE_DIR = path.join(process.cwd(), '.nvideo-cache');
const DEFAULT_CACHE_TTL = 5 * 60 * 1000; // 5 minutes

function computeCacheKey(inputPath, config) {
    const stat = fs.statSync(inputPath);
    const mtime = stat.mtimeMs.toString();
    const configCopy = Object.assign({}, config);
    delete configCopy._outputPath;
    const configStr = JSON.stringify(configCopy);
    const input = inputPath + mtime + configStr;
    return crypto.createHash('sha256').update(input).digest('hex');
}

function getCacheFilePath(cacheDir, cacheKey, outputPath) {
    const ext = path.extname(outputPath) || '.dat';
    return path.join(cacheDir, cacheKey + ext);
}

function loadCacheMetadata(cacheDir) {
    const metaPath = path.join(cacheDir, 'cache.json');
    if (fs.existsSync(metaPath)) {
        try {
            return JSON.parse(fs.readFileSync(metaPath, 'utf8'));
        } catch (e) {
            return {};
        }
    }
    return {};
}

function saveCacheMetadata(cacheDir, metadata) {
    if (!fs.existsSync(cacheDir)) {
        fs.mkdirSync(cacheDir, { recursive: true });
    }
    const metaPath = path.join(cacheDir, 'cache.json');
    fs.writeFileSync(metaPath, JSON.stringify(metadata, null, 2));
}

// Clean up expired and retrieved entries, return updated metadata
function cleanCache(metadata, cacheDir, ttl) {
    const now = Date.now();
    let changed = false;
    
    for (const [key, entry] of Object.entries(metadata)) {
        if (key === 'cache.json') continue;
        
        // Retrieved entries are deleted immediately (transmit-once pattern)
        if (entry.retrievedAt) {
            const cacheFile = path.join(cacheDir, key);
            if (fs.existsSync(cacheFile)) fs.unlinkSync(cacheFile);
            delete metadata[key];
            changed = true;
            continue;
        }
        
        // Expired entries
        if (ttl > 0 && (now - entry.createdAt) > ttl) {
            const cacheFile = path.join(cacheDir, key);
            if (fs.existsSync(cacheFile)) fs.unlinkSync(cacheFile);
            delete metadata[key];
            changed = true;
        }
    }
    
    if (changed) saveCacheMetadata(cacheDir, metadata);
    return metadata;
}

function lookupCache(inputPath, config, cacheDir, ttl) {
    const cacheKey = computeCacheKey(inputPath, config);
    
    if (!fs.existsSync(cacheDir)) {
        return { hit: false, cacheKey, cacheFile: null };
    }
    
    // Clean expired/retrieved entries first
    const metadata = loadCacheMetadata(cacheDir);
    cleanCache(metadata, cacheDir, ttl);
    
    // Search for cache file with any extension
    const files = fs.readdirSync(cacheDir);
    const cacheFile = files.find(f => f.startsWith(cacheKey + '.') && f !== 'cache.json');
    if (cacheFile) {
        return { hit: true, cacheKey, cacheFile: path.join(cacheDir, cacheFile) };
    }
    return { hit: false, cacheKey, cacheFile: null };
}

function storeInCache(cacheFile, sourcePath, cacheDir) {
    if (!fs.existsSync(cacheDir)) {
        fs.mkdirSync(cacheDir, { recursive: true });
    }
    
    fs.copyFileSync(sourcePath, cacheFile);
    
    const metadata = loadCacheMetadata(cacheDir);
    const stat = fs.statSync(cacheFile);
    metadata[path.basename(cacheFile)] = {
        inputPath: path.resolve(cacheFile),
        createdAt: Date.now(),
        size: stat.size
    };
    saveCacheMetadata(cacheDir, metadata);
    
    return cacheFile;
}

function copyFromCache(cacheFile, outputPath, cacheDir) {
    fs.copyFileSync(cacheFile, outputPath);
    
    // Mark as retrieved — will be deleted on next cache operation (transmit-once)
    const metadata = loadCacheMetadata(cacheDir);
    const basename = path.basename(cacheFile);
    if (metadata[basename]) {
        metadata[basename].retrievedAt = Date.now();
        saveCacheMetadata(cacheDir, metadata);
    }
    
    return fs.statSync(outputPath).size;
}

function clearCache(opts = {}) {
    const cacheDir = opts.cacheDir || DEFAULT_CACHE_DIR;
    
    if (!fs.existsSync(cacheDir)) {
        return { cleared: 0 };
    }
    
    const metadata = loadCacheMetadata(cacheDir);
    const now = Date.now();
    const olderThan = opts.olderThan || 0;
    let cleared = 0;
    
    for (const [key, entry] of Object.entries(metadata)) {
        // Retrieved entries are always deleted (transmit-once pattern)
        if (entry.retrievedAt) {
            const cacheFile = path.join(cacheDir, key);
            if (fs.existsSync(cacheFile)) fs.unlinkSync(cacheFile);
            delete metadata[key];
            cleared++;
            continue;
        }
        
        if (olderThan > 0 && (now - entry.createdAt) < olderThan) {
            continue;
        }
        
        const cacheFile = path.join(cacheDir, key);
        if (fs.existsSync(cacheFile)) {
            fs.unlinkSync(cacheFile);
        }
        delete metadata[key];
        cleared++;
    }
    
    if (cleared > 0) {
        saveCacheMetadata(cacheDir, metadata);
    }
    
    return { cleared };
}

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

// Convenience layer: nVideo.probe(path) - ffprobe equivalent
function probe(filePath) {
    if (typeof filePath !== 'string') {
        throw new TypeError('probe: expected string argument (file path)');
    }
    return nativeBinding.probe(filePath);
}

// Convenience layer: nVideo.getMetadata(path) - lightweight metadata only
function getMetadata(filePath) {
    if (typeof filePath !== 'string') {
        throw new TypeError('getMetadata: expected string argument (file path)');
    }
    return nativeBinding.getMetadata(filePath);
}

// Convenience layer: nVideo.thumbnail(path, opts) - extract video frame as RGB
function thumbnail(filePath, opts) {
    if (typeof filePath !== 'string') {
        throw new TypeError('thumbnail: expected string argument (file path)');
    }
    if (!opts || typeof opts !== 'object') {
        throw new TypeError('thumbnail: expected object argument (opts)');
    }
    if (typeof opts.timestamp !== 'number') {
        throw new TypeError('thumbnail: opts.timestamp must be a number (seconds)');
    }
    if (typeof opts.width !== 'number') {
        throw new TypeError('thumbnail: opts.width must be a number');
    }
    return nativeBinding.thumbnail(filePath, opts);
}

// AudioInput - wraps native audio/decoder instance
class AudioInput {
    constructor(nativeInput) {
        this._input = nativeInput;
        this._videoOpened = false;
    }

    getDuration() {
        return this._input.getDuration();
    }

    getSampleRate() {
        return this._input.getSampleRate();
    }

    getChannels() {
        return this._input.getChannels();
    }

    getTotalSamples() {
        return this._input.getTotalSamples();
    }

    getCodecName() {
        return this._input.getCodecName();
    }

    getInputSampleRate() {
        return this._input.getInputSampleRate();
    }

    getInputChannels() {
        return this._input.getInputChannels();
    }

    isOpen() {
        return this._input.isOpen();
    }

    seek(seconds) {
        if (typeof seconds !== 'number') {
            throw new TypeError('seek: expected number argument (seconds)');
        }
        return this._input.seek(seconds);
    }

    // Read audio samples - returns { samples, data } where data is Float32Array
    readAudio(numSamples) {
        if (typeof numSamples !== 'number') {
            throw new TypeError('readAudio: expected number argument (numSamples)');
        }
        return this._input.readAudio(numSamples);
    }

    // ==================== Video Decode ====================

    // Open video stream for frame reading
    openVideoStream(streamIndex = -1) {
        this._videoOpened = this._input.openVideoStream(streamIndex);
        return this._videoOpened;
    }

    getVideoWidth() {
        return this._input.getVideoWidth();
    }

    getVideoHeight() {
        return this._input.getVideoHeight();
    }

    getVideoFPS() {
        return this._input.getVideoFPS();
    }

    getVideoCodecName() {
        return this._input.getVideoCodecName();
    }

    getPosition() {
        return this._input.getPosition();
    }

    // Read next video frame - caller provides buffer
    // Returns frame info { width, height, pts, duration, frameNum, keyframe, format }
    // Data is written directly into the provided buffer (zero-copy)
    readVideoFrame(buffer, opts = {}) {
        if (!buffer || !(buffer instanceof Uint8Array)) {
            throw new TypeError('readVideoFrame: expected Uint8Array buffer');
        }
        return this._input.readVideoFrame(buffer, opts);
    }

    // ==================== Waveform Generation ====================

    // Get waveform - blocking, returns peak amplitudes for L/R channels
    // Returns { peaksL: Float32Array, peaksR: Float32Array, points, duration }
    getWaveform(numPoints) {
        if (typeof numPoints !== 'number') {
            throw new TypeError('getWaveform: expected number argument (numPoints)');
        }
        return this._input.getWaveform(numPoints);
    }

    // Get waveform with streaming progress callback
    // numPoints: number of data points in waveform
    // chunkSizeMB: size in MB between progress callbacks (e.g., 10 = every 10MB)
    // onProgress: callback function(result) - return false to abort early
    getWaveformStreaming(numPoints, chunkSizeMB, onProgress) {
        if (typeof numPoints !== 'number') {
            throw new TypeError('getWaveformStreaming: expected number argument (numPoints)');
        }
        if (typeof chunkSizeMB !== 'number') {
            throw new TypeError('getWaveformStreaming: expected number argument (chunkSizeMB)');
        }
        if (typeof onProgress !== 'function') {
            throw new TypeError('getWaveformStreaming: expected function argument (onProgress)');
        }
        return this._input.getWaveformStreaming(numPoints, chunkSizeMB, onProgress);
    }

    close() {
        this._input.close();
    }
}

// Convenience layer: nVideo.openInput(path, opts) - open audio file for decoding
function openInput(filePath, opts) {
    if (typeof filePath !== 'string') {
        throw new TypeError('openInput: expected string argument (file path)');
    }
    const nativeInput = nativeBinding.openInput(filePath, opts || {});
    return new AudioInput(nativeInput);
}

// Export the convenience functions along with native binding
module.exports = {
    version: nativeBinding.version(),
    probe,
    getMetadata,
    thumbnail,
    openInput,
    AudioInput,
    // Expose native binding for advanced usage
    native: nativeBinding,

    // ==================== Transcode to File ====================

    // Transcode - full re-encode with progress reporting
    // nVideo.transcode(input, output, {
    //   video: { codec: 'libx264', width: 1280, height: 720, crf: 23, preset: 'medium' },
    //   audio: { codec: 'aac', bitrate: 128000 },
    //   threads: 4,
    //   onProgress: (p) => {},
    //   onComplete: (r) => {},
    //   onError: (e) => {}
    // })
    transcode(inputPath, outputPath, opts) {
        if (typeof inputPath !== 'string') {
            throw new TypeError('transcode: expected string argument (inputPath)');
        }
        if (typeof outputPath !== 'string') {
            throw new TypeError('transcode: expected string argument (outputPath)');
        }
        if (!opts || typeof opts !== 'object') {
            throw new TypeError('transcode: expected object argument (opts)');
        }

        const cacheEnabled = opts.cache !== false;
        const cacheDir = opts.cacheDir || DEFAULT_CACHE_DIR;
        const cacheTTL = opts.cacheTTL !== undefined ? opts.cacheTTL : DEFAULT_CACHE_TTL;

        if (cacheEnabled) {
            const cacheConfig = { video: opts.video, audio: opts.audio, threads: opts.threads };
            const cacheResult = lookupCache(inputPath, cacheConfig, cacheDir, cacheTTL);

            if (cacheResult.hit) {
                if (opts.onCacheHit) {
                    opts.onCacheHit(cacheResult.cacheFile);
                }
                copyFromCache(cacheResult.cacheFile, outputPath, cacheDir);
                const stat = fs.statSync(outputPath);
                return { duration: 0, frames: 0, audioFrames: 0, size: stat.size, bitrate: 0, speed: 0, timeMs: 0, dupFrames: 0, dropFrames: 0, cached: true };
            }

            if (opts.onCacheMiss) {
                opts.onCacheMiss();
            }
        }

        const result = nativeBinding.transcode(inputPath, outputPath, opts);

        if (cacheEnabled && result) {
            const cacheConfig = { video: opts.video, audio: opts.audio, threads: opts.threads };
            const cacheKey = computeCacheKey(inputPath, cacheConfig);
            const cacheFile = getCacheFilePath(cacheDir, cacheKey, outputPath);
            storeInCache(cacheFile, outputPath, cacheDir);
        }

        return result;
    },

    // Remux - stream copy without re-encode (fast)
    // nVideo.remux(input, output, {
    //   onProgress: (p) => {},
    //   onComplete: (r) => {},
    //   onError: (e) => {}
    // })
    remux(inputPath, outputPath, opts) {
        if (typeof inputPath !== 'string') {
            throw new TypeError('remux: expected string argument (inputPath)');
        }
        if (typeof outputPath !== 'string') {
            throw new TypeError('remux: expected string argument (outputPath)');
        }

        const finalOpts = opts || {};
        const cacheEnabled = finalOpts.cache !== false;
        const cacheDir = finalOpts.cacheDir || DEFAULT_CACHE_DIR;
        const cacheTTL = finalOpts.cacheTTL !== undefined ? finalOpts.cacheTTL : DEFAULT_CACHE_TTL;

        if (cacheEnabled) {
            const cacheConfig = { operation: 'remux' };
            const cacheResult = lookupCache(inputPath, cacheConfig, cacheDir, cacheTTL);

            if (cacheResult.hit) {
                if (finalOpts.onCacheHit) {
                    finalOpts.onCacheHit(cacheResult.cacheFile);
                }
                copyFromCache(cacheResult.cacheFile, outputPath, cacheDir);
                const stat = fs.statSync(outputPath);
                return { duration: 0, frames: 0, audioFrames: 0, size: stat.size, bitrate: 0, speed: 0, timeMs: 0, dupFrames: 0, dropFrames: 0, cached: true };
            }

            if (finalOpts.onCacheMiss) {
                finalOpts.onCacheMiss();
            }
        }

        const result = nativeBinding.remux(inputPath, outputPath, finalOpts);

        if (cacheEnabled && result) {
            const cacheConfig = { operation: 'remux' };
            const cacheKey = computeCacheKey(inputPath, cacheConfig);
            const cacheFile = getCacheFilePath(cacheDir, cacheKey, outputPath);
            storeInCache(cacheFile, outputPath, cacheDir);
        }

        return result;
    },

    // Convert - shorthand with auto-detected defaults
    // nVideo.convert(input, output, {
    //   video: { codec: 'libx264', crf: 23 },
    //   audio: { codec: 'aac' }
    // })
    convert(inputPath, outputPath, opts) {
        if (typeof inputPath !== 'string') {
            throw new TypeError('convert: expected string argument (inputPath)');
        }
        if (typeof outputPath !== 'string') {
            throw new TypeError('convert: expected string argument (outputPath)');
        }
        // Default to H.264 + AAC if not specified
        const finalOpts = Object.assign({}, opts || {});
        if (!finalOpts.video) {
            finalOpts.video = { codec: 'libx264', crf: 23 };
        }
        if (!finalOpts.audio) {
            finalOpts.audio = { codec: 'aac', bitrate: 128000 };
        }
        return this.transcode(inputPath, outputPath, finalOpts);
    },

    // ==================== Convenience Functions ====================

    // Concat - join multiple files into one
    // nVideo.concat(['file1.mp4', 'file2.mp4'], 'output.mp4', opts)
    concat(files, outputPath, opts = {}) {
        if (!Array.isArray(files)) {
            throw new TypeError('concat: expected array argument (files)');
        }
        if (typeof outputPath !== 'string') {
            throw new TypeError('concat: expected string argument (outputPath)');
        }
        return nativeBinding.concat(files, outputPath, opts);
    },

    // ExtractStream - extract a single stream to a new file
    // nVideo.extractStream('input.mp4', 'output.aac', 1) // extract audio stream
    extractStream(inputPath, outputPath, streamIndex, opts = {}) {
        if (typeof inputPath !== 'string') {
            throw new TypeError('extractStream: expected string argument (inputPath)');
        }
        if (typeof outputPath !== 'string') {
            throw new TypeError('extractStream: expected string argument (outputPath)');
        }
        if (typeof streamIndex !== 'number') {
            throw new TypeError('extractStream: expected number argument (streamIndex)');
        }
        return nativeBinding.extractStream(inputPath, outputPath, streamIndex, opts);
    },

    // ExtractAudio - decode audio from video, re-encode to target format
    // nVideo.extractAudio('video.mp4', 'output.wav')
    // nVideo.extractAudio('video.mp4', 'output.mp3', { codec: 'libmp3lame', bitrate: 320000 })
    extractAudio(inputPath, outputPath, opts = {}) {
        if (typeof inputPath !== 'string') {
            throw new TypeError('extractAudio: expected string argument (inputPath)');
        }
        if (typeof outputPath !== 'string') {
            throw new TypeError('extractAudio: expected string argument (outputPath)');
        }

        const cacheEnabled = opts.cache !== false;
        const cacheDir = opts.cacheDir || DEFAULT_CACHE_DIR;
        const cacheTTL = opts.cacheTTL !== undefined ? opts.cacheTTL : DEFAULT_CACHE_TTL;

        if (cacheEnabled) {
            const cacheConfig = { operation: 'extractAudio', codec: opts.codec, bitrate: opts.bitrate };
            const cacheResult = lookupCache(inputPath, cacheConfig, cacheDir, cacheTTL);

            if (cacheResult.hit) {
                if (opts.onCacheHit) {
                    opts.onCacheHit(cacheResult.cacheFile);
                }
                copyFromCache(cacheResult.cacheFile, outputPath, cacheDir);
                const stat = fs.statSync(outputPath);
                return { duration: 0, frames: 0, audioFrames: 0, size: stat.size, bitrate: 0, speed: 0, timeMs: 0, dupFrames: 0, dropFrames: 0, cached: true };
            }

            if (opts.onCacheMiss) {
                opts.onCacheMiss();
            }
        }

        const result = nativeBinding.extractAudio(inputPath, outputPath, opts);

        if (cacheEnabled && result) {
            const cacheConfig = { operation: 'extractAudio', codec: opts.codec, bitrate: opts.bitrate };
            const cacheKey = computeCacheKey(inputPath, cacheConfig);
            const cacheFile = getCacheFilePath(cacheDir, cacheKey, outputPath);
            storeInCache(cacheFile, outputPath, cacheDir);
        }

        return result;
    },

    // ==================== Streaming Players ====================

    // AudioStreamPlayer - SAB ring buffer + AudioWorklet playback
    // const player = new nVideo.AudioStreamPlayer(audioContext);
    // await player.open('audio.mp3');
    // player.play();
    AudioStreamPlayer: require('./player-audio').AudioStreamPlayer,

    // VideoStreamPlayer - frame queue + canvas rendering
    // const player = new nVideo.VideoStreamPlayer({ canvas: myCanvas });
    // await player.open('video.mp4');
    // player.play();
    VideoStreamPlayer: require('./player-video').VideoStreamPlayer,

    // ==================== Cache Management ====================

    // Clear cache entries
    // nVideo.clearCache() - remove all
    // nVideo.clearCache({ olderThan: 7 * 24 * 60 * 60 * 1000 }) - older than 7 days
    // nVideo.clearCache({ cacheDir: './custom-cache' }) - custom directory
    clearCache(opts) {
        return clearCache(opts || {});
    }
};
