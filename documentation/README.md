# nVideo API Documentation

**nVideo** is a native Node.js module for video/audio processing via N-API and FFmpeg. It provides zero-copy decode, transcode, streaming, thumbnails, waveforms, and more.

## Table of Contents

- [Quick Start](#quick-start)
- [Core Concepts](#core-concepts)
- [API Reference](#api-reference)
  - [Probe & Metadata](#probe--metadata)
  - [Transcode](#transcode)
  - [Streaming Decode](#streaming-decode)
  - [Streaming Playback](#streaming-playback)
  - [Capabilities](#capabilities)
  - [Caching](#caching)
- [Classes](#classes)
  - [AudioInput](#audioinput)
  - [AudioStreamPlayer](#audiostreamplayer)
  - [VideoStreamPlayer](#videostreamplayer)
  - [BufferPool](#bufferpool)
  - [RingBuffer](#ringbuffer)
  - [AVStreamPlayer](#avstreamplayer)
- [Configuration](#configuration)
- [Error Handling](#error-handling)
- [Performance Tips](#performance-tips)
- [Hardware Acceleration](#hardware-acceleration)
- [Electron Support](#electron-support)

---

## Quick Start

```javascript
const nVideo = require('nvideo');

// Probe a file
const info = nVideo.probe('video.mp4');
console.log(info.format.name, info.duration);

// Transcode with progress
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'libx264', width: 1280, height: 720 },
  audio: { codec: 'aac', bitrate: 128000 },
  onProgress: (p) => console.log(p.percent + '%')
});

// Stream audio decode
const input = nVideo.openInput('audio.mp3');
const { samples, data } = input.readAudio(4096); // Float32Array
```

---

## Core Concepts

### Two Data Paths

nVideo operates in two distinct modes:

1. **Transcode Path** (90% of usage): File → File. Entire pipeline runs in C++ on async worker thread. Zero frames touch V8 heap. Node.js initiates operation and receives completion callback.

2. **Streaming Path** (real-time playback): Frame-by-frame decode with zero-copy into caller's buffer. Used for AudioWorklet/VideoFrame consumption in Electron.

### Zero-Copy Design

- Transcode to file: FFmpeg reads → decodes → filters → encodes → writes directly to disk
- Stream to JS: Decoded frames written directly into caller's `Uint8Array`/`Float32Array` or `SharedArrayBuffer`

### Threading

- Transcode operations run on async worker threads (via `napi_threadsafe_function`)
- Progress callbacks happen on main thread
- Streaming decode is synchronous but designed for efficient main thread usage

---

## API Reference

### Probe & Metadata

#### `nVideo.probe(path)`

Returns comprehensive file metadata (ffprobe equivalent).

**Parameters:**
- `path` (string): Path to media file

**Returns:** `ProbeResult`
```typescript
interface ProbeResult {
  format: {
    name: string;        // e.g., "mov,mp4,m4a,3gp,3g2,mj2"
    longName: string;    // e.g., "QuickTime / MOV"
    duration: number;    // seconds
    size: number;        // bytes
    bitrate: number;     // bits/sec
  };
  tags: Record<string, string>;  // Metadata tags
  streams: StreamInfo[];         // Array of stream information
}

interface StreamInfo {
  index: number;
  type: 'video' | 'audio' | 'subtitle' | 'data';
  codec: string;         // e.g., "h264"
  codecLong: string;     // e.g., "H.264 / AVC / MPEG-4 AVC"
  bitrate: number;
  // Video-specific:
  width?: number;
  height?: number;
  fps?: number;
  pixelFormat?: string;  // e.g., "yuv420p"
  // Audio-specific:
  sampleRate?: number;
  channels?: number;
  channelLayout?: string; // e.g., "stereo"
  bitsPerSample?: number;
  tags: Record<string, string>;
}
```

**Example:**
```javascript
const info = nVideo.probe('movie.mp4');
console.log(`${info.format.name}: ${info.format.duration}s`);

const videoStream = info.streams.find(s => s.type === 'video');
console.log(`${videoStream.width}x${videoStream.height} @ ${videoStream.fps}fps`);
```

#### `nVideo.getMetadata(path)`

Lightweight metadata extraction (faster than probe for just tags).

**Returns:** `Record<string, string>` - Metadata key-value pairs

---

### Thumbnails

#### `nVideo.thumbnail(path, opts)`

Extract a single video frame as RGB24 data.

**Parameters:**
- `path` (string): Path to video file
- `opts` (object):
  - `timestamp` (number): Position in seconds
  - `width` (number): Target width in pixels (height auto-calculated)

**Returns:**
```typescript
interface ThumbnailResult {
  data: Uint8Array;    // RGB24 pixel data
  width: number;
  height: number;
  pts: number;         // Actual timestamp of decoded frame
  keyframe: boolean;   // Whether this was a keyframe
  format: 'rgb24';
}
```

**Example:**
```javascript
const thumb = nVideo.thumbnail('video.mp4', { timestamp: 10.0, width: 320 });
fs.writeFileSync('thumb.raw', thumb.data); // Save raw RGB data
```

---

### Transcode

#### `nVideo.transcode(inputPath, outputPath, opts)`

Full re-encode with progress reporting. Supports hardware acceleration.

**Parameters:**
- `inputPath` (string): Input file path
- `outputPath` (string): Output file path
- `opts` (object):
  - `video` (VideoOpts | null | undefined): Video encoding options (null = copy, undefined = default)
  - `audio` (AudioOpts | null | undefined): Audio encoding options
  - `threads` (number): Number of threads (0 = auto)
  - `hwaccel` (string): Hardware acceleration type ('cuda', 'qsv', 'vaapi', etc.)
  - `onProgress` (function): Progress callback
  - `onComplete` (function): Completion callback
  - `onError` (function): Error callback

**VideoOpts:**
```typescript
interface VideoOpts {
  codec: string;        // e.g., 'libx264', 'h264_nvenc', 'copy'
  width?: number;       // 0 = keep original
  height?: number;      // 0 = keep original
  crf?: number;         // Quality (for x264/x265), -1 = default
  preset?: string;      // e.g., 'medium', 'fast', 'slow'
  pixelFormat?: string; // e.g., 'yuv420p'
  bitrate?: number;     // Target bitrate in bits/sec
  fps?: number;         // Force frame rate
  filters?: string;     // FFmpeg filter graph string
}
```

**AudioOpts:**
```typescript
interface AudioOpts {
  codec: string;        // e.g., 'aac', 'libmp3lame', 'copy'
  bitrate?: number;     // Target bitrate in bits/sec
  sampleRate?: number;  // Output sample rate
  channels?: number;    // Output channels
  filters?: string;     // FFmpeg filter graph string
}
```

**Progress Callback Data:**
```typescript
interface TranscodeProgress {
  time: number;              // Current timestamp (seconds)
  percent: number;           // 0-100
  speed: number;             // Processing speed (1.0 = realtime)
  bitrate: number;           // Current output bitrate
  size: number;              // Bytes written
  frames: number;            // Video frames encoded
  fps: number;               // Current throughput (frames/sec)
  audioFrames: number;       // Audio samples encoded
  audioTime: number;         // Audio timestamp
  estimatedDuration: number; // Total input duration
  estimatedSize: number;     // Projected final size
  eta: number;               // Seconds remaining
  dupFrames: number;         // Duplicate frames
  dropFrames: number;        // Dropped frames
}
```

**Example:**
```javascript
const result = await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'libx264', width: 1920, height: 1080, crf: 23, preset: 'medium' },
  audio: { codec: 'aac', bitrate: 128000 },
  threads: 4,
  onProgress: (p) => {
    console.log(`${p.percent.toFixed(1)}% - ${p.speed.toFixed(2)}x speed`);
  }
});
console.log(`Done: ${result.size} bytes in ${result.timeMs}ms`);
```

#### `nVideo.remux(inputPath, outputPath, opts)`

Stream copy without re-encode (fast container swap).

**Example:**
```javascript
await nVideo.remux('input.mkv', 'output.mp4', {
  onProgress: (p) => console.log(p.percent + '%')
});
```

#### `nVideo.convert(inputPath, outputPath, opts)`

Shorthand with auto-detected defaults (H.264 + AAC).

**Example:**
```javascript
// Same as transcode with { video: { codec: 'libx264', crf: 23 }, audio: { codec: 'aac', bitrate: 128000 } }
await nVideo.convert('input.avi', 'output.mp4');
```

#### `nVideo.concat(files, outputPath, opts)`

Join multiple files into one (stream copy).

**Parameters:**
- `files` (string[]): Array of input file paths
- `outputPath` (string): Output file path
- `opts` (object): Optional callbacks

**Example:**
```javascript
await nVideo.concat(['part1.mp4', 'part2.mp4', 'part3.mp4'], 'combined.mp4');
```

#### `nVideo.extractStream(inputPath, outputPath, streamIndex, opts)`

Extract a single stream to a new file.

**Example:**
```javascript
// Extract audio stream (index 1) to separate file
await nVideo.extractStream('video.mp4', 'audio.aac', 1);
```

#### `nVideo.extractAudio(inputPath, outputPath, opts)`

Decode audio from video and re-encode to target format.

**Example:**
```javascript
// Extract to WAV
await nVideo.extractAudio('video.mp4', 'audio.wav');

// Extract to MP3 with specific settings
await nVideo.extractAudio('video.mp4', 'audio.mp3', {
  codec: 'libmp3lame',
  bitrate: 320000
});
```

---

### Streaming Decode

#### `nVideo.openInput(path, opts)`

Open a file for streaming audio/video decode. Returns an `AudioInput` instance.

**Parameters:**
- `path` (string): File path
- `opts` (object):
  - `sampleRate` (number): Target audio sample rate (default: 44100)
  - `threads` (number): Decoder threads (default: 0 = auto)

**Returns:** [AudioInput](#audioinput) instance

**Example:**
```javascript
const input = nVideo.openInput('audio.mp3', { sampleRate: 48000 });

// Get metadata
console.log(input.getDuration(), input.getSampleRate(), input.getChannels());

// Read audio chunks
while (input.isOpen()) {
  const { samples, data } = input.readAudio(4096); // Returns Float32Array
  if (samples === 0) break;
  // Process data (samples * channels Float32 values)
}

input.close();
```

---

### Streaming Playback

#### `nVideo.AudioStreamPlayer`

AudioWorklet-based player with SharedArrayBuffer ring buffer. See [AudioStreamPlayer](#audiostreamplayer) class docs.

#### `nVideo.VideoStreamPlayer`

Canvas-based video player with frame queue. See [VideoStreamPlayer](#videostreamplayer) class docs.

#### `nVideo.AVStreamPlayer`

Synchronized audio + video playback. See [AVStreamPlayer](#avstreamplayer) class docs.

---

### Capabilities

#### `nVideo.getBuildInfo()`

Returns FFmpeg build information (live from binary).

**Returns:**
```typescript
interface BuildInfo {
  version: string;              // FFmpeg version string
  configuration: string;        // Build configuration flags
  hwaccels: string[];           // Available hardware accelerations
  protocols: string[];          // Supported protocols
}
```

**Example:**
```javascript
const info = nVideo.getBuildInfo();
console.log(info.hwaccels); // ['cuda', 'nvenc', 'qsv', ...]
```

#### `nVideo.getCodecs(type?)`

Returns available codecs (live from binary).

**Parameters:**
- `type` (string, optional): Filter by type ('video', 'audio', 'subtitle')

**Returns:** Array of codec objects

**Example:**
```javascript
const videoEncoders = nVideo.getCodecs('video')
  .filter(c => c.canEncode && c.name.includes('nvenc'));
```

#### `nVideo.getFilters(type?)`

Returns available filters (live from binary).

**Example:**
```javascript
const videoFilters = nVideo.getFilters('video');
const scaleFilter = videoFilters.find(f => f.name === 'scale');
```

#### `nVideo.getFormats()`

Returns available container formats.

**Example:**
```javascript
const formats = nVideo.getFormats();
const mp4 = formats.find(f => f.name.includes('mp4'));
console.log(mp4.canMux, mp4.canDemux, mp4.extensions);
```

#### `nVideo.getCapabilities()`

Returns pre-generated capability data from JSON files (fast, no native module call).

**Example:**
```javascript
const caps = nVideo.getCapabilities();
console.log(caps.commonCodecs.encoders.video.nvidia); // NVENC encoders
console.log(caps.commonCodecs.recommended.modern);    // { video: 'libsvtav1', audio: 'libopus' }
```

---

### Caching

#### `nVideo.clearCache(opts?)`

Clear cached transcode outputs.

**Parameters:**
- `opts` (object):
  - `cacheDir` (string): Custom cache directory
  - `olderThan` (number): Only clear entries older than X milliseconds

**Example:**
```javascript
// Clear all cache
nVideo.clearCache();

// Clear entries older than 7 days
nVideo.clearCache({ olderThan: 7 * 24 * 60 * 60 * 1000 });
```

---

## Classes

### AudioInput

Stateful audio/video decoder instance created by `nVideo.openInput()`.

#### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `getDuration()` | number | Total duration in seconds |
| `getSampleRate()` | number | Output sample rate (Hz) |
| `getChannels()` | number | Output channels (usually 2) |
| `getTotalSamples()` | number | Total sample count |
| `getCodecName()` | string | Source codec name |
| `getInputSampleRate()` | number | Source sample rate |
| `getInputChannels()` | number | Source channels |
| `isOpen()` | boolean | Whether input is open |
| `seek(seconds)` | boolean | Seek to timestamp |
| `readAudio(numSamples)` | {samples, data} | Read interleaved float32 audio |
| `openVideoStream(index?)` | boolean | Open video stream for decode |
| `getVideoWidth()` | number | Video width in pixels |
| `getVideoHeight()` | number | Video height in pixels |
| `getVideoFPS()` | number | Video frame rate |
| `getVideoCodecName()` | string | Video codec name |
| `getPosition()` | number | Current position in seconds |
| `readVideoFrame(buffer, opts?)` | FrameInfo | Decode frame into buffer |
| `getWaveform(numPoints)` | WaveformResult | Generate waveform data |
| `getWaveformStreaming(numPoints, chunkSizeMB, callback)` | WaveformResult | Streaming waveform with progress |
| `close()` | void | Close input and free resources |

**Audio Read Example:**
```javascript
const input = nVideo.openInput('music.mp3');
const sampleRate = input.getSampleRate(); // 44100
const channels = input.getChannels();     // 2

while (true) {
  const { samples, data } = input.readAudio(4096);
  if (samples === 0) break;

  // data is Float32Array with samples * channels elements
  // Interleaved: [L0, R0, L1, R1, L2, R2, ...]
  for (let i = 0; i < samples; i++) {
    const left = data[i * 2];
    const right = data[i * 2 + 1];
    // Process samples...
  }
}
input.close();
```

**Video Read Example:**
```javascript
const input = nVideo.openInput('video.mp4');
input.openVideoStream(); // Open first video stream

const width = input.getVideoWidth();
const height = input.getVideoHeight();
const buffer = new Uint8Array(width * height * 3); // RGB24

while (true) {
  const frame = input.readVideoFrame(buffer, { format: 'rgb24' });
  if (!frame.width) break; // EOF

  console.log(`Frame ${frame.frameNum} @ ${frame.pts}s`);
  // buffer now contains RGB24 pixel data
}
input.close();
```

---

### AudioStreamPlayer

AudioWorklet-based player for gapless, low-latency audio playback.

**Note:** Requires `AudioContext` with `audioWorklet` support (Chrome/Electron).

#### Constructor

```javascript
const player = new nVideo.AudioStreamPlayer(audioContext, opts);
```

**Parameters:**
- `audioContext` (AudioContext): Web Audio API context
- `opts` (object):
  - `bufferDuration` (number): Ring buffer duration in seconds (default: 2.0)
  - `channels` (number): Audio channels (default: 2)

#### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `open(input)` | Promise<void> | Open AudioInput instance |
| `play()` | void | Start playback |
| `pause()` | void | Pause playback |
| `stop()` | void | Stop and reset |
| `seek(seconds)` | void | Seek to position |
| `setLoop(enabled, start?, end?)` | void | Enable/disable looping |
| `getCurrentTime()` | number | Get playback position |
| `getState()` | string | Get state: 'playing', 'paused', 'stopped' |
| `isReady()` | boolean | Whether player is initialized |
| `disconnect()` | void | Disconnect from audio graph |

**Example:**
```javascript
const audioContext = new AudioContext();
const player = new nVideo.AudioStreamPlayer(audioContext);

const input = nVideo.openInput('audio.mp3');
await player.open(input);

player.play();

// Later...
player.seek(30.0); // Seek to 30 seconds
player.setLoop(true, 10.0, 20.0); // Loop 10-20 seconds
```

---

### VideoStreamPlayer

Canvas-based video player with frame queue and SAB ring buffer.

#### Constructor

```javascript
const player = new nVideo.VideoStreamPlayer(opts);
```

**Parameters:**
- `opts` (object):
  - `canvas` (HTMLCanvasElement): Target canvas
  - `openInput` (function): Reference to `nVideo.openInput` (for Electron main process)
  - `maxQueuedFrames` (number): Frame queue size (default: 8)

#### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `open(input)` | Promise<void> | Open input (path string or AudioInput) |
| `play()` | void | Start playback |
| `pause()` | void | Pause playback |
| `stop()` | void | Stop and reset |
| `seek(seconds)` | void | Seek to position |
| `setLoop(enabled, start?, end?)` | void | Enable/disable looping |
| `getCurrentTime()` | number | Get estimated position |
| `getState()` | string | Get playback state |
| `getVideoInfo()` | object | Get width, height, fps, duration |
| `setTargetFps(fps)` | void | Override render frame rate |
| `disconnect()` | void | Clean up resources |

**Example:**
```javascript
const canvas = document.getElementById('videoCanvas');
const player = new nVideo.VideoStreamPlayer({ canvas });

await player.open('video.mp4');
player.play();

// Get video info
const info = player.getVideoInfo();
console.log(`${info.width}x${info.height} @ ${info.fps}fps`);
```

---

### BufferPool

Pre-allocated buffers for zero-GC streaming loops.

#### Constructor

```javascript
const pool = new nVideo.createBufferPool(opts);
```

**Parameters:**
- `opts` (object):
  - `audioBufferSize` (number): Audio buffer size in samples (default: 8192)
  - `audioBufferCount` (number): Number of audio buffers (default: 8)
  - `videoBufferSize` (number): Video buffer size in bytes (default: 1920*1080*3)
  - `videoBufferCount` (number): Number of video buffers (default: 8)

#### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `acquireAudio(size)` | Float32Array | Get audio buffer |
| `releaseAudio(buf)` | void | Return buffer to pool |
| `acquireVideo(size)` | Uint8Array | Get video buffer |
| `releaseVideo(buf)` | void | Return buffer to pool |
| `stats` | object | Get pool statistics |
| `dispose()` | void | Free all buffers |

**Example:**
```javascript
const pool = nVideo.createBufferPool({
  audioBufferSize: 4096,
  audioBufferCount: 16,
  videoBufferSize: 1920 * 1080 * 3,
  videoBufferCount: 8
});

const input = nVideo.openInput('video.mp4');
input.openVideoStream();

while (input.isOpen()) {
  const buf = pool.acquireVideo(input.getVideoWidth() * input.getVideoHeight() * 3);
  const frame = input.readVideoFrame(buf);
  if (!frame.width) break;

  // Process frame...

  pool.releaseVideo(buf);
}

console.log(pool.stats); // { audioTotal: 16, audioAcquired: 0, ... }
pool.dispose();
```

---

### RingBuffer

Pure JS ring buffer for main process (no SharedArrayBuffer needed).

#### Constructor

```javascript
const ring = new nVideo.createRingBuffer(capacity, channels);
```

**Parameters:**
- `capacity` (number): Frame capacity
- `channels` (number): Audio channels

#### Properties & Methods

| Member | Type | Description |
|--------|------|-------------|
| `available` | number | Frames available to read |
| `space` | number | Frames that can be written |
| `empty` | boolean | Whether buffer is empty |
| `full` | boolean | Whether buffer is full |
| `write(src, frames)` | number | Write frames to buffer |
| `read(dst, frames)` | number | Read frames from buffer |
| `clear()` | void | Clear buffer |

**Example:**
```javascript
const ring = nVideo.createRingBuffer(4096, 2); // 4096 frames, stereo

// Write
const written = ring.write(audioData, 1024);

// Read
const out = new Float32Array(1024 * 2);
const read = ring.read(out, 1024);
```

---

### AVStreamPlayer

Synchronized audio + video playback (combines AudioStreamPlayer + VideoStreamPlayer).

#### Constructor

```javascript
const av = new nVideo.AVStreamPlayer(opts);
```

**Parameters:**
- `opts` (object):
  - `audioContext` (AudioContext): Web Audio context
  - `canvas` (HTMLCanvasElement): Video canvas
  - `openInput` (function): Reference to `nVideo.openInput`

#### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `openAudio(path, opts?)` | Promise<void> | Open audio file |
| `openVideo(path, opts?)` | Promise<void> | Open video file |
| `play()` | void | Start synchronized playback |
| `pause()` | void | Pause both audio and video |
| `stop()` | void | Stop playback |
| `seek(seconds)` | void | Seek to position |
| `setLoop(enabled, start?, end?)` | void | Enable looping |
| `getCurrentTime()` | number | Get audio position (master clock) |
| `getState()` | string | Get playback state |

**Example:**
```javascript
const av = new nVideo.AVStreamPlayer({
  audioContext: new AudioContext(),
  canvas: document.getElementById('canvas')
});

await av.openAudio('movie.mp4');
await av.openVideo('movie.mp4');

av.play();
```

---

## Configuration

### Environment Variables

| Variable | Description |
|----------|-------------|
| `NVIDEO_CACHE_DIR` | Default cache directory |
| `NVIDEO_CACHE_TTL` | Default cache TTL in milliseconds |

### Transcode Options Reference

**Video Codecs:**
- CPU: `libx264`, `libx265`, `libsvtav1`, `libaom-av1`
- NVIDIA: `h264_nvenc`, `hevc_nvenc`, `av1_nvenc`
- Intel: `h264_qsv`, `hevc_qsv`, `av1_qsv`
- AMD: `h264_amf`, `hevc_amf`, `av1_amf`

**Audio Codecs:**
- `aac`, `libfdk_aac` (best AAC)
- `libmp3lame` (MP3)
- `flac` (lossless)
- `libopus` (best modern)
- `ac3`, `eac3` (Dolby Digital)
- `pcm_s16le`, `pcm_s24le` (uncompressed)

**Pixel Formats:**
- `yuv420p` (most compatible)
- `yuv422p` (broadcast)
- `yuv444p` (highest quality)
- `rgb24`, `bgr24` (uncompressed)

**Presets (x264/x265):**
- `ultrafast`, `superfast`, `veryfast`, `faster`, `fast`
- `medium` (default)
- `slow`, `slower`, `veryslow` (best quality)

---

## Error Handling

All async operations (transcode, remux, etc.) throw on error. Wrap in try/catch:

```javascript
try {
  await nVideo.transcode('input.mp4', 'output.mp4', opts);
} catch (err) {
  console.error('Transcode failed:', err.message);
}
```

Use `onError` callback for granular error handling:

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  onError: (err) => {
    console.error(`${err.operation} failed at ${err.timestamp}s: ${err.message}`);
  }
});
```

---

## Performance Tips

1. **Use remux for format conversion** - 100x faster than transcode when codec is compatible
2. **Match output to input** - Don't upscale or change FPS unnecessarily
3. **Hardware encoding** - Use NVENC/QSV/AMF for real-time encoding (slightly lower quality)
4. **Thread count** - Set to physical core count for best performance
5. **Caching** - Enable cache for repeated operations on same files
6. **Buffer reuse** - Use BufferPool for streaming to avoid GC pressure
7. **Zero-copy** - Provide your own buffers to `readVideoFrame()` and `readAudio()`

---

## Hardware Acceleration

### NVIDIA NVENC

Requires NVIDIA GPU with NVENC support (GTX 600+ or RTX series).

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'h264_nvenc', bitrate: 5000000 },
  hwaccel: 'cuda'
});
```

### Intel QSV

Requires Intel CPU with integrated graphics (4th Gen+).

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'h264_qsv' },
  hwaccel: 'qsv'
});
```

### AMD AMF

Requires AMD GPU (RX 400+ series).

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'h264_amf' }
});
```

Check available hardware accelerations:

```javascript
const hwaccels = nVideo.getBuildInfo().hwaccels;
console.log(hwaccels); // ['cuda', 'nvenc', 'qsv', 'amdgpu', ...]
```

---

## Electron Support

### Installation

```bash
npm install nvideo
npx electron-rebuild -f -w nvideo
```

### CSP Headers (for SharedArrayBuffer)

Add to your Electron main process:

```javascript
const { session } = require('electron');

session.defaultSession.webRequest.onHeadersReceived((details, callback) => {
  callback({
    responseHeaders: {
      ...details.responseHeaders,
      'Cross-Origin-Opener-Policy': ['same-origin'],
      'Cross-Origin-Embedder-Policy': ['require-corp']
    }
  });
});
```

### Main vs Renderer Process

- **Main process**: Use all nVideo APIs directly
- **Renderer process**: Use `ipcRenderer` to call main process, or use streaming players

---

## License

MIT © David Renelt
