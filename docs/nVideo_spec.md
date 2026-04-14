# nVideo Architecture Specification

**Last Updated**: 2026-04-13

**Version**: 0.2.0 - Two-Pipeline Architecture

## Vision

nVideo is a native Node.js module (N-API) that brings FFmpeg's full media processing capabilities to JavaScript. It is the video/media counterpart to nImage — where nImage makes exotic image formats accessible through a native API, nVideo makes professional-grade video and audio processing accessible through a native FFmpeg interface.

### The Problem

Video and audio processing in Node.js today means one of three things:

1. **Spawning `ffmpeg` CLI** — process overhead per call, string-based API, no streaming, no zero-copy. Every frame or audio chunk requires serialization through stdin/stdout pipes or temporary files.
2. **fluent-ffmpeg** — a well-designed JS wrapper around the CLI. Still suffers from process spawn overhead, no direct memory access, no real-time streaming. Method chaining creates intermediate objects that add GC pressure.
3. **Writing your own native addon** — every project re-implements the same FFmpeg integration boilerplate.

None of these approaches give you direct memory access to decoded video frames or audio samples. None support real-time streaming pipelines where decoded data flows from FFmpeg into a `SharedArrayBuffer` for zero-copy consumption by Web Audio or Canvas.

### The Solution

nVideo provides a single native module that:

- **Probes** media files instantly — metadata, streams, codec info, without spawning a process
- **Extracts** thumbnails and waveforms without full decode
- **Transcodes** directly to file — FFmpeg reads, processes, and writes entirely in C++ memory. Node.js initiates the operation and gets a callback when done. No frames touch JavaScript memory.
- **Streams** decoded video frames and audio samples directly into JavaScript memory without copies — for real-time playback in Electron via SharedArrayBuffer
- **Transforms** via FFmpeg's filter graph — any filter, any chain, using FFmpeg's native filter graph syntax

### Why N-API? (When to use nVideo vs ffmpeg CLI)

Not every operation needs N-API. The key distinction is whether decoded data needs to flow through JavaScript memory:

| Use Case | Needs N-API? | Why |
|----------|-------------|-----|
| Metadata / probe | Nice to have | Faster than spawning ffprobe, no process overhead |
| Thumbnail extraction | Nice to have | Avoids process spawn + temp files |
| Waveform generation | Nice to have | Avoids process spawn overhead |
| Transcode to file | Nice to have | Avoids process spawn, but both work |
| Stream audio to AudioWorklet | **Required** | Zero-copy into SAB, impossible from CLI |
| Stream video to VideoFrame/Canvas | **Required** | Zero-copy into SAB, impossible from CLI |

The N-API addon exists because of the bottom two rows — they are impossible with a CLI wrapper. But the top four rows are where 90% of usage happens, and they're the reason someone picks nVideo over just spawning `ffmpeg.exe`.

### Design Principles

1. **Zero-copy by default** — decoded data is written directly into JS-owned memory. No intermediate copies between FFmpeg and JavaScript.
2. **File output bypasses JS entirely** — when transcoding to file, FFmpeg's native I/O writes directly to disk. The entire read → decode → filter → encode → write pipeline runs in C++ memory. Node.js only initiates the operation and receives a completion callback. No decoded frames ever enter V8 heap.
3. **Pure C++ core, thin N-API wrapper** — the `FFmpegProcessor` class has zero N-API dependencies. The binding layer is a thin translation membrane.
4. **FFmpeg-native API** — the JS API maps 1:1 to FFmpeg's C API and CLI. Existing FFmpeg recipes translate directly. Filter graphs use FFmpeg's filter graph syntax. Codec options map to `AVOptions`.
5. **Performance-first JS surface** — no method chaining, no intermediate objects. The API uses flat function calls and configuration objects. A thin JS convenience layer may wrap the FFmpeg-native core, but it must not introduce allocation overhead.
6. **No CLI dependency** — links directly against FFmpeg's C libraries (`libavformat`, `libavcodec`, `libswscale`, `libswresample`, `libavfilter`). No `ffmpeg.exe` needed.
7. **Rich progress reporting** — every long-running operation reports detailed, real-time progress back to JavaScript. Progress data mirrors what FFmpeg's `-progress` flag provides: timestamps, frame counts, bitrates, speed, ETA. Progress is delivered via callbacks from async workers (`napi_threadsafe_function`), never EventEmitter.
8. **Electron-compatible** — MSVC-built binaries, proper DLL loading, tested in renderer and main process.
9. **Checkpoint before debugging** — when you encounter problems, always make a commit before starting to change things, so you have a clean state to return to when you actually figure out the cause/solution.
10. **When in doubt, ask the Human** — otherwise, this is the agent's project. Be as smart and creative as possible, and have fun!

### Relationship to Reference Projects

| Aspect | nImage | ffmpeg-napi-interface | nVideo |
|--------|--------|----------------------|--------|
| Domain | Image decode | Audio decode | Video + Audio |
| Native libs | LibRaw, LibHeif | libavcodec, libavformat, libswresample | Full FFmpeg stack + libswscale |
| Zero-copy | Input only (output copies) | Full (Float32Array direct write) | Full (video frames + audio) |
| API style | Sharp-compatible pipeline | Class-based decoder | FFmpeg-native |
| Encoding | Via Sharp | Not yet | Native FFmpeg encode |
| Streaming | Partial (tile decode) | SharedArrayBuffer ring buffer | Frame-by-frame + SAB |
| Video support | No | No | Core feature |
| File output | N/A | N/A | Bypasses JS entirely |

nVideo takes the **architectural separation** from ffmpeg-napi-interface (pure C++ core, thin N-API) and extends it into the full video domain. The API mirrors FFmpeg's own C API and CLI — existing FFmpeg recipes translate directly to nVideo calls. A thin JS convenience layer provides ergonomic helpers without chaining or allocation overhead.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 3: JavaScript API (lib/index.js)                             │
│  - Utility: nVideo.probe(), nVideo.thumbnail(), nVideo.waveform()   │
│  - Transcode: nVideo.transcode() — fire and forget, C++ writes disk │
│  - Streaming: frame-by-frame decode into SAB for playback           │
│  - Convenience: nVideo.remux(), nVideo.concat(), nVideo.convert()   │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 2: N-API Bindings (src/binding.cpp)                          │
│  - ProcessorWrapper (Napi::ObjectWrap)                              │
│  - Zero-copy data marshalling (Float32Array, Uint8Array)            │
│  - Type conversion, validation, error handling                      │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 1: Native Core (src/processor.h/.cpp)                        │
│  - FFmpegProcessor class (pure C++, no N-API)                       │
│  - libavformat: container demuxing/muxing                           │
│  - libavcodec: decode/encode                                        │
│  - libswscale: pixel format conversion, scaling                     │
│  - libswresample: audio resampling, format conversion               │
│  - libavfilter: filter graph (resize, crop, rotate, EQ, etc.)      │
└─────────────────────────────────────────────────────────────────────┘
```

### Two Pipelines

nVideo has two fundamentally different pipelines with different goals, APIs, and performance characteristics:

#### Pipeline A: Transcode (File → File) — PRIMARY

**Goal**: Maximum performance file-to-file transcoding. Equivalent to `ffmpeg` CLI but with better progress, hardware acceleration support, and optional caching.

**Use cases**: Transcode, remux, convert, concat, extract audio, extract stream — any operation where input is a file path and output is a file path.

**Design**:
- Input: file path(s) + configuration object
- Output: file path (success/failure + stats)
- Entire pipeline runs in C++ on async worker thread
- Zero frames touch JavaScript memory
- Supports hardware acceleration (NVENC, QSV, VAAPI) via codec selection
- Optional caching: hash(input_path + config) → cached output path
- Progress via callbacks (`napi_threadsafe_function`)

```
Node.js                          C++ (FFmpeg)
───────                          ────────────
nVideo.transcode(                ┌─────────────────────┐
  'input.mkv',                   │ avformat_open_input │
  'output.mp4',                  │ av_read_frame       │
  { video: {...} }  ──────────► │ avcodec_decode      │
);                               │ avfilter (scale)    │
                                 │ avcodec_encode      │
callback()  ◄────────────────── │ av_interleaved_write│
                                 │ av_write_trailer    │
                                 └─────────────────────┘
                                          │
                                          ▼
                                    output.mp4 (disk)

Zero frames pass through V8 heap.
Node.js thread is free during processing (async worker).
```

**Operations**:
| Function | Description | CLI Equivalent |
|----------|-------------|----------------|
| `transcode(input, output, opts)` | Full re-encode with filters | `ffmpeg -i in -vf ... -c:v ... out` |
| `remux(input, output)` | Stream copy, no re-encode | `ffmpeg -i in -c copy out` |
| `convert(input, output, opts)` | Shorthand with auto-defaults | `ffmpeg -i in out` |
| `concat(files, output)` | Join multiple files | `ffmpeg -f concat -i list.txt out` |
| `extractStream(input, output, index)` | Copy single stream | `ffmpeg -i in -map 0:a:0 -c copy out` |
| `extractAudio(input, output, opts)` | Decode + re-encode audio | `ffmpeg -i video.mp4 -vn -c:a pcm_s16le out.wav` |

#### Pipeline B: Streaming (Chunk → Chunk) — FUTURE

**Goal**: Frame-by-frame decode into caller-owned buffers for real-time processing. Primary use case: TTS/STT services, audio analysis.

**Use cases**: Real-time audio decode for speech-to-text, video frame extraction for analysis, AudioWorklet playback.

**Design**:
- Input: `openInput(path)` → decoder instance
- Output: `readAudio(buffer)` / `readVideoFrame(buffer)` → decoded data in caller's buffer
- Zero-copy: C++ writes directly into caller's `Float32Array` / `Uint8Array`
- Caller controls decode loop, timing, and buffer allocation
- Supports seek, filter graphs, pixel format selection

```
Node.js                          C++ (FFmpeg)
───────                          ────────────
                                 ┌─────────────────────┐
input.readVideoFrame(buf) ─────► │ av_read_frame       │
            │                     │ avcodec_decode      │
            │                     │ sws_scale ──────────┼──► buf (V8 memory)
            ▼                     └─────────────────────┘
        { data, pts }
            │
            ▼
      SharedArrayBuffer ──────────► AudioWorklet / VideoFrame
      (zero-copy to renderer)
```

**Operations**:
| Function | Description |
|----------|-------------|
| `openInput(path)` | Open file, returns decoder instance |
| `input.openDecoder(streamIndex)` | Initialize decoder for specific stream |
| `input.readAudio(numSamples)` | Decode + resample into Float32Array |
| `input.readVideoFrame(buffer)` | Decode + scale into Uint8Array |
| `input.seek(seconds)` | Jump to timestamp |
| `input.close()` | Release resources |
### Dual Audio Pipeline

Audio processing uses two fundamentally different approaches depending on the data path:

**Path A (Transcode to file)** — Always uses FFmpeg's filter graph (`abuffer → [user filters] → aformat → abuffersink`). The filter graph handles resampling, frame sizing, channel layout conversion, and timestamp propagation internally. This is the correct approach because FFmpeg manages the entire audio frame lifecycle, producing correctly-timestamped frames that match the encoder's requirements exactly. The `aformat` filter ensures output matches the encoder's sample format, sample rate, and channel layout.

```
abuffer (decoder output) → [user filters, e.g. volume=0.5] → aformat (encoder format) → abuffersink → encoder
```

**Path B (Streaming to JS)** — Uses `SwrContext` (libswresample) directly for manual frame-by-frame resampling into caller-owned buffers. This gives the caller explicit control over buffer allocation and timing, which is required for real-time AudioWorklet/SharedArrayBuffer consumption where the caller drives the decode loop.

The manual SWR+chunking approach is **not used** for transcode-to-file because it requires reinventing frame sizing, PTS propagation, and buffer management that FFmpeg's filter graph already handles correctly. The filter graph approach eliminated the audio encoding bugs (missing timestamps, incorrect bitrate, frame_size warnings) that the manual chunking code produced.

## Project Structure

```
nVideo/
├── src/
│   ├── processor.h          # FFmpegProcessor class (pure C++)
│   ├── processor.cpp        # FFmpegProcessor implementation
│   ├── binding.cpp          # N-API bindings (thin wrapper)
│   └── utils.h/.cpp         # Helper functions
├── lib/
│   ├── index.js             # JS entry point, convenience layer
│   ├── player-video.js      # Video streaming player (SAB + VideoFrame)
│   └── player-audio.js      # Audio streaming player (SAB + AudioWorklet)
├── deps/
│   └── ffmpeg/              # Pre-built FFmpeg shared libs (BtbN)
│       ├── include/         # Headers
│       ├── win/             # Windows .lib + .dll
│       └── linux/           # Linux .so
├── dist/                    # Pre-built .node binaries (per platform)
├── scripts/
│   ├── download-ffmpeg.js   # Download FFmpeg from BtbN
│   └── package-binary.js    # Create release tarball
├── test/
│   ├── video.test.js        # Video decode/encode tests
│   ├── audio.test.js        # Audio decode/encode tests
│   ├── streaming.test.js    # Real-time streaming tests
│   ├── benchmark.js         # Performance benchmarks
│   └── assets/              # Test media files
├── binding.gyp              # node-gyp build configuration
├── package.json
├── nVideo_spec.md           # This file
└── AGENTS.md                # LLM development guide
```

## Feature Scope

### Pipeline A: Transcode (File → File) — PRIMARY

These are the highest-value features. Fast, fire-and-forget, minimal JS involvement. Justify nVideo over ffmpeg CLI.

#### Tier 1: Core Operations (Done or Near-Done)

| Feature | Status | Description | Equivalent CLI |
|---------|--------|-------------|----------------|
| `probe()` | ✅ Done | Metadata, streams, codec info, tags | `ffprobe` |
| `thumbnail()` | ✅ Done | Seek + decode single frame, return RGB | `ffmpeg -ss t -i f -vframes 1` |
| `waveform()` | ✅ Done | Full audio decode, peak amplitudes | (custom) |
| `transcode()` | ⚠️ Near | Full re-encode pipeline, file→file | `ffmpeg -i in -c:v ... -c:a ... out` |
| `remux()` | ✅ Done | Stream copy, no re-encode | `ffmpeg -i in -c copy out` |
| `convert()` | ✅ Done | Shorthand with auto-defaults | `ffmpeg -i in out` |

#### Tier 2: Transcode Enhancements (Next)

| Feature | Status | Description |
|---------|--------|-------------|
| `extractAudio()` | ⬜ TODO | Decode + re-encode audio from video (e.g., video.mp4 → output.wav/mp3) |
| Hardware acceleration | ⬜ TODO | NVENC, QSV, VAAPI codec selection in transcode options |
| CRF/preset support | ⚠️ Broken | `av_opt_set` linking issue with BtbN FFmpeg build |
| Caching system | ⬜ TODO | Hash(input_path + config) → cached output, skip redundant work |
| Concat fix | ✅ Done | Manual per-file processing with cumulative DTS offset tracking |
| Transcode benchmark | ⬜ TODO | Prove parity with ffmpeg CLI performance |

#### Tier 3: Advanced Transcode

| Feature | Description |
|---------|-------------|
| Complex filter graphs | `-filter_complex` equivalent, multi-input/output |
| Stream mapping | Select/duplicate/remap streams (FFmpeg `-map`) |
| Subtitle extraction/burn-in | Extract subtitle tracks, burn into video |
| HDR tone mapping | HDR10/HLG → SDR conversion |

### Pipeline B: Streaming (Chunk → Chunk) — COMPLETE

Frame-level decode for real-time processing. Primary use case: audio/video playback in Electron.

| Feature | Status | Description |
|---------|--------|-------------|
| `openInput()` / `close()` | ✅ Done | Open file, returns decoder instance |
| `readAudio()` | ✅ Done | Zero-copy decode into Float32Array |
| `readVideoFrame()` | ✅ Done | Zero-copy decode into Uint8Array |
| `seek()` | ✅ Done | Jump to timestamp |
| AudioWorklet player | ✅ Done | SAB ring buffer + AudioWorklet for playback |
| VideoFrame player | ✅ Done | SAB frame queue + Canvas rendering |
| Buffer pool | ✅ Done | Pre-allocated buffers, zero GC pressure |
| Synchronized A/V | ✅ Done | Locked audio/video clock for playback |

## Zero-Copy Strategy

### File Output — Zero JS Involvement

For `transcode()`, `remux()`, `convert()`, and any file-to-file operation, the entire pipeline runs in C++:

```cpp
// C++ side: open input, open output, pump frames, write to disk
// Node.js is NOT in the loop. Runs on async worker thread.
bool FFmpegProcessor::transcode(const char* inputPath, const char* outputPath, const TranscodeOptions& opts) {
    // avformat_open_input(inputPath)
    // avformat_alloc_output_context2(outputPath)
    // loop: av_read_frame → avcodec_decode → avfilter → avcodec_encode → av_write_frame
    // av_write_trailer
    // avformat_close_input
}
```

Node.js calls `nVideo.transcode()`, which launches an `Napi::AsyncWorker`. The worker runs the entire FFmpeg pipeline on a background thread. Node.js receives a callback when done. No decoded data ever crosses the N-API boundary.

### Audio — Zero-Copy into JS Memory (proven pattern from ffmpeg-napi-interface)

```cpp
// Caller provides Float32Array, C++ writes directly into it
Napi::Float32Array buffer = Napi::Float32Array::New(env, numSamples);
int samplesRead = processor->readAudio(buffer.Data(), numSamples);
// swr_convert writes directly into V8-managed memory
```

### Video — Zero-Copy into JS Memory

```cpp
// Caller provides Uint8Array, C++ writes directly into it
Napi::Uint8Array frame = Napi::Uint8Array::New(env, width * height * 4);
processor->readVideoFrame(frame.Data(), width, height, &actualWidth, &actualHeight);
// sws_scale writes directly into V8-managed memory
```

### SharedArrayBuffer Streaming (proven pattern from ffmpeg-napi-interface)

```
Main Thread                          Renderer (AudioWorklet / VideoFrame)
    │                                        │
    │  ┌─────────────────────────────┐       │
    │  │   SharedArrayBuffer         │       │
    │  │   ┌───────┬───────┬──────┐  │       │
    │  │   │ Control│ Audio │ Video │  │       │
    │  │   │ (meta) │ Ring  │ Queue │  │       │
    │  │   └───────┴───────┴──────┘  │       │
    │  └─────────────────────────────┘       │
    │         ▲ write              read ▼     │
    │    decoder.read()           process()   │
```

## Transcode Pipeline Details

### Caching System

File-to-file operations can be expensive. The caching system avoids redundant work by hashing the input file + configuration and reusing previous output.

**Cache key**: `SHA256(input_path + input_mtime + JSON.stringify(config))`

**Cache storage**: `.nvideo-cache/` directory (configurable)
```
.nvideo-cache/
├── abc123def456...mp4    # cached output file
├── 789xyz012abc...mkv
└── cache.json            # metadata: { key → { inputPath, createdAt, size, retrievedAt? } }
```

**Transmit-once TTL**: Cache entries have a default 5-minute TTL. Once retrieved, the entry is marked for immediate deletion on the next cache operation. This prevents stale cache accumulation while supporting the common pattern of "transcode once, use the output, discard the cache."

**API**:
```javascript
// Cache enabled by default for transcode/convert/remux/extractAudio
nVideo.transcode('input.mkv', 'output.mp4', {
  cache: true,              // default: true
  cacheTTL: 5 * 60 * 1000,  // default: 5 minutes (0 = infinite)
  cacheDir: './.nvideo-cache',
  video: { codec: 'libx264', crf: 23 },
  onCacheHit: (cachedPath) => console.log('Cache hit:', cachedPath),
  onCacheMiss: () => console.log('Cache miss, transcoding...')
});

// Disable cache for one-off operations
nVideo.transcode('input.mkv', 'output.mp4', {
  cache: false,
  video: { codec: 'libx264' }
});

// Infinite TTL (cache persists until retrieved or manually cleared)
nVideo.transcode('input.mkv', 'output.mp4', {
  cacheTTL: 0,
  video: { codec: 'libx264' }
});

// Clear cache
nVideo.clearCache();        // remove all cached files (including retrieved)
nVideo.clearCache({ olderThan: 7 * 24 * 60 * 60 * 1000 }); // older than 7 days
```

**Cache invalidation**:
- Input file modified time changes → cache miss
- Input file deleted → cache miss
- Config differs → cache miss
- Cache entry older than TTL → cache miss, entry deleted (default: 5 minutes)
- Cache entry was retrieved → deleted on next lookup (transmit-once pattern)

**Cache behavior**:
- On cache hit: copy cached file to requested output path (fast), mark entry as retrieved, return immediately
- On cache miss: transcode normally, store output in cache, copy to output path
- Retrieved entries are deleted on the next cache operation (lookup or clearCache)
- Expired entries are cleaned up during lookup — no timers, no background work
- `cacheTTL: 0` disables TTL expiry (entries persist until retrieved or manually cleared)

### Hardware Acceleration

Hardware encoders are selected via the `codec` option, matching FFmpeg's encoder names. nVideo does not auto-detect hardware — the caller specifies which encoder to use.

**Supported hardware encoders**:
| Encoder | Platform | Codec | Notes |
|---------|----------|-------|-------|
| `h264_nvenc` | NVIDIA GPU | H.264 | Fast, good quality |
| `hevc_nvenc` | NVIDIA GPU | H.265 | 4K, HDR |
| `av1_nvenc` | NVIDIA RTX 40xx | AV1 | Next-gen |
| `h264_qsv` | Intel Quick Sync | H.264 | Integrated GPU |
| `hevc_qsv` | Intel Quick Sync | H.265 | 4K |
| `h264_vaapi` | Linux VA-API | H.264 | Intel/AMD on Linux |
| `hevc_vaapi` | Linux VA-API | H.265 | 4K |

**API**:
```javascript
// NVIDIA GPU encoding
nVideo.transcode('input.mkv', 'output.mp4', {
  video: { codec: 'h264_nvenc', width: 1920, height: 1080, preset: 'p4', cq: 23 },
  audio: { codec: 'aac', bitrate: 128000 }
});

// Intel Quick Sync
nVideo.transcode('input.mkv', 'output.mp4', {
  video: { codec: 'h264_qsv', width: 1280, height: 720 },
  audio: { codec: 'aac' }
});

// Hardware decode + encode (full GPU pipeline)
nVideo.transcode('input.mkv', 'output.mp4', {
  hwaccel: 'cuda',            // hardware decode: 'cuda', 'qsv', 'vaapi', or null
  video: { codec: 'h264_nvenc', width: 1920, height: 1080 },
  audio: { codec: 'aac' }
});
```

**Hardware decode** (`hwaccel` option):
- `'cuda'` — NVIDIA GPU decode (requires CUDA-capable GPU)
- `'qsv'` — Intel Quick Sync decode
- `'vaapi'` — Linux VA-API decode
- `null` — software decode (default)

When `hwaccel` is set, nVideo adds the appropriate FFmpeg flags (`-hwaccel cuda`, `-hwaccel_device 0`, etc.) and uses hardware pixel formats (`cuda`, `qsv`, `vaapi_vld`) throughout the pipeline.

## Supported Formats

FFmpeg supports 100+ codecs and 50+ containers. Key formats include:

### Video Codecs

| Codec | Decode | Encode | Notes |
|-------|--------|--------|-------|
| H.264/AVC | Yes | Yes | Most common |
| H.265/HEVC | Yes | Yes | 4K, HDR |
| VP8 | Yes | Yes | WebM |
| VP9 | Yes | Yes | WebM, HDR |
| AV1 | Yes | Yes | Next-gen |
| ProRes | Yes | Yes | Professional |
| MPEG-2 | Yes | Yes | Broadcast |
| MPEG-4 | Yes | Yes | Legacy |
| DNxHD/DNxHR | Yes | Yes | Avid |
| FFV1 | Yes | Yes | Lossless |
| Raw video | Yes | Yes | Uncompressed |

### Audio Codecs

| Codec | Decode | Encode | Notes |
|-------|--------|--------|-------|
| AAC | Yes | Yes | Universal |
| MP3 | Yes | Yes | Legacy |
| FLAC | Yes | Yes | Lossless |
| Opus | Yes | Yes | Streaming |
| Vorbis | Yes | Yes | WebM |
| PCM | Yes | Yes | WAV/AIFF |
| AC3/EAC3 | Yes | Yes | Surround |
| AMR | Yes | Yes | Voice |

### Container Formats

| Format | Demux | Mux | Notes |
|--------|-------|-----|-------|
| MP4 | Yes | Yes | Universal |
| MKV/WebM | Yes | Yes | Flexible |
| MOV | Yes | Yes | Apple |
| AVI | Yes | Yes | Legacy |
| TS/MTS | Yes | Yes | Broadcast |
| FLV | Yes | Yes | Streaming |
| WAV/AIFF | Yes | Yes | Audio |
| OGG | Yes | Yes | Audio |

## API Design (Proposed)

The API mirrors FFmpeg's C API and CLI. Every FFmpeg recipe has a direct nVideo equivalent. Filter graphs use FFmpeg's native filter graph syntax. Codec options map 1:1 to FFmpeg's `AVOptions`.

### Utility Functions (Primary API — Phase 1)

These are the bread and butter. Fast, fire-and-forget, minimal JS involvement.

```javascript
const nVideo = require('nvideo');

// Probe — ffprobe equivalent, no decode
const info = nVideo.probe('input.mp4');
// → {
//     format: { name: 'mov,mp4', duration: 245.7, bitrate: 4500000, size: 138000000 },
//     streams: [
//       { index: 0, type: 'video', codec: 'h264', width: 1920, height: 1080,
//         fps: 29.97, bitrate: 4000000, pixelFormat: 'yuv420p' },
//       { index: 1, type: 'audio', codec: 'aac', sampleRate: 48000,
//         channels: 2, bitrate: 128000 }
//     ],
//     tags: { title: 'My Video', encoder: 'Lavf60.3.100' }
//   }

// Thumbnail — seek + decode single frame, return pixel data
const thumb = nVideo.thumbnail('input.mp4', { timestamp: 5.0, width: 320 });
// → { data: Uint8Array (RGB), width: 320, height: 180, format: 'rgb24' }

// Waveform — full audio decode, peak amplitudes
const waveform = nVideo.waveform('input.mp4', { points: 1000 });
// → { peaksL: Float32Array, peaksR: Float32Array, duration: 245.7 }

// Waveform with progress (runs on async worker for large files)
nVideo.waveform('input.mp4', {
  points: 1000,
  onProgress: (p) => {
    // p.time = 125.4       — current audio timestamp processed
    // p.percent = 51.0     — progress percentage
    // p.speed = 3.2        — decode speed (3.2x realtime)
    // p.eta = 38.0         — estimated seconds remaining
    console.log(`Waveform: ${p.percent.toFixed(1)}%`);
  },
  onComplete: (result) => {
    // result.peaksL, result.peaksR, result.duration
  }
});
```

### Transcode to File (Zero JS Memory Involvement)

The entire pipeline runs in C++ on a background thread. FFmpeg reads from disk, processes, writes to disk. Node.js only initiates and receives a completion callback.

```javascript
// Synchronous (blocks, simple)
nVideo.transcode('input.mkv', 'output.mp4', {
  video: { codec: 'libx264', width: 1280, height: 720, crf: 23, preset: 'medium' },
  audio: { codec: 'aac', bitrate: 128000 }
});

// Async (non-blocking, with detailed progress)
nVideo.transcode('input.mkv', 'output.mp4', {
  video: { codec: 'libx264', width: 1280, height: 720, crf: 23, preset: 'medium' },
  audio: { codec: 'aac', bitrate: 128000 },
  onProgress: (progress) => {
    console.log(progress);
  },
  onComplete: (result) => console.log('Done', result),
  onError: (err) => console.error(err)
});

// onProgress receives (called from C++ async worker via threadsafe_function):
// {
//   time: 125.4,              // current timestamp processed (seconds)
//   percent: 51.0,            // progress percentage (0-100)
//   speed: 2.5,               // processing speed (1.0 = realtime, 2.5 = 2.5x faster)
//   bitrate: 3200000,         // current output bitrate (bits/sec)
//   size: 50000000,           // current output file size (bytes)
//   frames: 3750,             // video frames processed
//   fps: 148.2,               // current encode throughput (frames/sec)
//   audioFrames: 5882400,     // audio samples processed
//   audioTime: 125.4,         // audio timestamp processed
//   estimatedDuration: 245.7, // total input duration (from probe)
//   estimatedSize: 138000000, // estimated final file size (bytes)
//   eta: 48.2                 // estimated seconds remaining
// }

// onComplete receives:
// {
//   duration: 245.7,          // total input duration
//   frames: 7350,             // total video frames encoded
//   audioFrames: 11764800,    // total audio samples encoded
//   size: 138000000,          // final output file size (bytes)
//   bitrate: 4490000,         // average output bitrate
//   speed: 2.3,               // average processing speed
//   timeMs: 106800            // total wall-clock time (ms)
// }
```

FFmpeg recipe equivalent:

```bash
# This CLI command:
ffmpeg -i input.mkv -vf "scale=1280:720" -c:v libx264 -crf 23 -preset medium -c:a aac -b:a 128k output.mp4

# Becomes this nVideo call:
nVideo.transcode('input.mkv', 'output.mp4', {
  video: { codec: 'libx264', width: 1280, height: 720, crf: 23, preset: 'medium' },
  audio: { codec: 'aac', bitrate: 128000 }
});
```

### More Convenience Functions

All file-to-file operations support the same progress callback pattern.

```javascript
// Remux — copy streams without re-encode
nVideo.remux('input.mkv', 'output.mp4', {
  onProgress: (p) => console.log(`${p.percent.toFixed(1)}% — ${p.size} bytes`),
  onComplete: (r) => console.log(`Done in ${(r.timeMs / 1000).toFixed(1)}s`)
});

// Concat — join files
nVideo.concat(['part1.mp4', 'part2.mp4', 'part3.mp4'], 'merged.mp4', {
  onProgress: (p) => console.log(`File ${p.fileIndex + 1}/${p.totalFiles} — ${p.percent.toFixed(1)}%`),
  onComplete: (r) => console.log(`Merged ${r.totalFiles} files, ${r.size} bytes`)
});
// concat progress adds: fileIndex, totalFiles (which input file is being processed)

// Extract single stream (stream copy, no re-encode)
nVideo.extractStream('input.mp4', 'output.aac', { streamIndex: 1 });

// Extract audio from video (decode + re-encode)
nVideo.extractAudio('video.mp4', 'output.wav');
// → { duration: 245.7, size: 43000000, timeMs: 12000 }

nVideo.extractAudio('video.mp4', 'output.mp3', {
  codec: 'libmp3lame', bitrate: 320000,
  onProgress: (p) => console.log(`${p.percent.toFixed(1)}%`)
});
// Equivalent to: ffmpeg -i video.mp4 -vn -c:a libmp3lame -b:a 320k output.mp3

// Convert — common format shorthand
nVideo.convert('input.wav', 'output.mp3', {
  audio: { codec: 'libmp3lame', bitrate: 320000 },
  onProgress: (p) => updateProgressBar(p.percent)
});
nVideo.convert('input.avi', 'output.mp4');  // auto-detect reasonable defaults
```

### Core API — Maps to FFmpeg C API (Phase 2)

For advanced use cases where utility functions aren't enough. Direct access to FFmpeg's decode/encode/mux pipeline.

```javascript
const nVideo = require('nvideo');

// avformat_open_input + avformat_find_stream_info
const input = nVideo.openInput('input.mp4');

// Inspect streams
console.log(input.streams);
// → [
//     { index: 0, type: 'video', codec: 'h264', width: 1920, height: 1080, ... },
//     { index: 1, type: 'audio', codec: 'aac', sampleRate: 48000, channels: 2, ... }
//   ]

// Open decoders for specific streams
input.openDecoder(0);  // video
input.openDecoder(1);  // audio

// Read decoded video frame (zero-copy into caller's buffer)
const frameBuffer = new Uint8Array(1920 * 1080 * 4);
const frame = input.readVideoFrame(frameBuffer);
// → { data: Uint8Array, width: 1920, height: 1080, format: 'yuv420p', pts: 0.041, ... }

// Read decoded audio (zero-copy into caller's Float32Array)
const audioBuffer = new Float32Array(4096);
const samplesRead = input.readAudio(audioBuffer);

// Seek
input.seek(30.0);

// Apply FFmpeg filter graph (native syntax, copy-paste from docs)
input.setVideoFilter('scale=1280:720:flags=lanczos,fps=30');
input.setAudioFilter('aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo');

// Manual output control
const output = nVideo.openOutput('output.mp4');
output.addStream({ type: 'video', codec: 'libx264', width: 1280, height: 720, crf: 23 });
output.addStream({ type: 'audio', codec: 'aac', bitrate: 128000 });
output.writeHeader();

let packet;
while ((packet = input.readPacket()) !== null) {
  output.writePacket(packet);
}

output.writeTrailer();
input.close();
output.close();
```

### Streaming Players — SAB Ring Buffer + AudioWorklet/Canvas (Phase B2/B3)

The differentiator. Impossible with CLI wrappers.

#### AudioStreamPlayer

```javascript
const nVideo = require('nvideo');

const audioContext = new AudioContext();
const player = new nVideo.AudioStreamPlayer(audioContext);

await player.open('song.mp3');
// → { duration: 245.7, sampleRate: 44100, channels: 2, totalFrames: 10814400 }

player.onEnded(() => console.log('Track finished'));
player.onPosition((time, duration) => console.log(`${time.toFixed(1)}s / ${duration.toFixed(1)}s`));

player.play();         // start playback
player.seek(30);       // jump to 30 seconds
player.pause();        // pause (disconnects worklet, saves CPU)
player.resume();       // resume
player.setLoop(true);  // gapless looping
player.setVolume(0.5); // set volume
player.getCurrentTime(); // → 30.5 (seconds)
player.dispose();      // release all resources
```

**Architecture**: AudioWorklet processor embedded as blob URL (no external file). SAB ring buffer with 12-slot Int32 control buffer. Drift-corrected setTimeout feed loop at 20ms. Worklet reuse across track switches when sample rate matches.

**Control Buffer Layout** (12 Int32 slots):
| Slot | Purpose |
|------|---------|
| `WRITE_PTR` | Main thread write position (frames) |
| `READ_PTR` | Worklet read position (frames) |
| `STATE` | 0=stopped, 1=playing, 2=paused |
| `SAMPLE_RATE` | Audio sample rate |
| `CHANNELS` | Channel count (always 2) |
| `LOOP_ENABLED` | 1=loop, 0=no loop |
| `LOOP_START` | Loop start frame |
| `LOOP_END` | Loop end frame |
| `TOTAL_FRAMES` | Frames to play (updated on seek/EOF) |
| `UNDERRUN_COUNT` | Buffer underrun counter |
| `START_TIME_HI` | Scheduled start time (float64 high 32 bits) |
| `START_TIME_LO` | Scheduled start time (float64 low 32 bits) |

#### VideoStreamPlayer

```javascript
const nVideo = require('nvideo');

const player = new nVideo.VideoStreamPlayer({
  canvas: document.getElementById('canvas'),
  width: 1280,
  height: 720
});

await player.open('video.mp4');
// → { duration: 119.9, width: 1280, height: 720, fps: 29.97, totalFrames: 3593 }

player.onFrame((frame) => {
  // frame = { frameNum, pts, keyframe, buffered }
});

player.play();
player.seek(60);
player.getCurrentTime(); // → 60.2 (seconds)
player.dispose();
```

**Architecture**: Frame queue + canvas 2D rendering. SAB ring buffer for decoded RGB frames. Separate feed loop (16ms) and render loop (at video FPS). Position tracking via frame number extrapolation.

#### AVStreamPlayer (Synchronized A/V)

```javascript
const nVideo = require('nvideo');

const av = new nVideo.AVStreamPlayer({
  audioContext: new AudioContext(),
  canvas: document.getElementById('canvas'),
  openInput: nVideo.openInput
});

await av.openAudio('movie.mp4');
await av.openVideo('movie.mp4');
av.play();
av.seek(60);
av.setLoop(true);
av.dispose();
```

#### BufferPool

```javascript
const pool = nVideo.createBufferPool({
  audioBufferSize: 8192,    // Float32Array size
  audioBufferCount: 8,      // Pre-allocated count
  videoBufferSize: 1920*1080*3, // Uint8Array size
  videoBufferCount: 8       // Pre-allocated count
});

const audioBuf = pool.acquireAudio(4096);
// ... use buffer ...
pool.releaseAudio(audioBuf);

console.log(pool.stats);
// → { audioTotal: 8, audioFree: 7, audioAcquired: 1, audioPeak: 1, ... }

pool.dispose();
```

### API ↔ FFmpeg C API Mapping

| nVideo | FFmpeg C API | FFmpeg CLI |
|--------|-------------|------------|
| `nVideo.probe(path)` | `avformat_find_stream_info` | `ffprobe` |
| `nVideo.thumbnail(path, opts)` | Seek + decode single frame | `-ss t -i path -vframes 1` |
| `nVideo.waveform(path, opts)` | Full audio decode + peak calc | (custom) |
| `nVideo.transcode(in, out, opts)` | Full pipeline (async worker) | `ffmpeg -i in ... out` |
| `nVideo.remux(in, out)` | Stream copy | `ffmpeg -i in -c copy out` |
| `nVideo.openInput(path)` | `avformat_open_input` + `avformat_find_stream_info` | `-i path` |
| `input.readPacket()` | `av_read_frame` | (internal) |
| `input.readVideoFrame(buf)` | `avcodec_send_packet` + `avcodec_receive_frame` + `sws_scale` | (internal) |
| `input.readAudio(buf)` | `avcodec_send_packet` + `avcodec_receive_frame` + `swr_convert` | (internal) |
| `input.seek(seconds)` | `av_seek_frame` | `-ss seconds` |
| `input.setVideoFilter(graph)` | `avfilter_graph_parse_ptr` | `-vf graph` |
| `input.setAudioFilter(graph)` | `avfilter_graph_parse_ptr` | `-af graph` |
| `nVideo.openOutput(path)` | `avformat_alloc_output_context2` | `path` (output) |
| `output.addStream(opts)` | `avcodec_find_encoder` + `avcodec_open2` + `avformat_new_stream` | `-c:v/-c:a codec` |
| `output.writeHeader()` | `avformat_write_header` | (implicit) |
| `output.writePacket(pkt)` | `avcodec_send_frame` + `avcodec_receive_packet` + `av_interleaved_write_frame` | (implicit) |
| `output.writeTrailer()` | `av_write_trailer` | (implicit) |

## JS Convenience Layer

The core API above is FFmpeg-native and maps 1:1. On top of that, a thin JS convenience layer provides ergonomic helpers. The design constraints:

- **No method chaining** — chaining creates intermediate objects per call, adding GC pressure in tight loops. Every `nVideo(input).resize().encode().save()` allocates wrapper objects that live just long enough to be GC'd.
- **No proxy objects** — no lazy evaluation, no deferred execution queues. Calls execute immediately.
- **Configuration objects, not builder patterns** — pass options as plain objects. Zero allocation beyond the object literal the caller already created.
- **Reusable buffers** — callers pass in their own `Uint8Array`/`Float32Array`. No hidden allocations.

### Why Not Chaining

```
// fluent-ffmpeg style — creates 4+ intermediate objects, each with closure captures
const result = nVideo(input)
    .resize(1280, 720)       // returns new wrapper #1
    .fps(30)                  // returns new wrapper #2
    .videoCodec('libx264')    // returns new wrapper #3
    .output(output);          // executes, discards wrappers

// nVideo style — one call, one config object, zero intermediate allocations
nVideo.transcode(input, output, {
    video: { codec: 'libx264', width: 1280, height: 720, fps: 30 }
});
```

In a tight render loop decoding 60 frames/second, chaining overhead matters. The fluent API is fine for one-shot transcode, but nVideo is designed for real-time streaming where every microsecond counts.

### Two-Layer Architecture

```
┌────────────────────────────────────────────────────────────┐
│  JS Convenience Layer (lib/index.js)                        │
│  - nVideo.transcode(), nVideo.remux(), nVideo.concat()      │
│  - nVideo.thumbnail(), nVideo.waveform(), nVideo.probe()    │
│  - Configuration objects, no chaining, no intermediate alloc │
├────────────────────────────────────────────────────────────┤
│  FFmpeg-Native Core (N-API bindings)                        │
│  - nVideo.openInput(), input.readVideoFrame(), input.seek() │
│  - nVideo.openOutput(), output.addStream(), output.write()  │
│  - 1:1 mapping to avformat/avcodec/avfilter C API           │
├────────────────────────────────────────────────────────────┤
│  Pure C++ (src/processor.h/.cpp)                            │
│  - FFmpegProcessor: zero N-API dependency                   │
│  - Direct libav* calls, zero-copy buffers                   │
│  - File-to-file transcode: entire pipeline in C++ memory    │
└────────────────────────────────────────────────────────────┘
```

The convenience layer is pure JS — no native code. It calls the FFmpeg-native API underneath. If a use case doesn't fit the convenience functions, drop down to the native API directly. No abstraction tax.

## Progress Reporting

Every long-running operation in nVideo provides detailed, real-time progress information. This is not an afterthought — it is a core design principle. FFmpeg's `-progress` flag outputs rich stats; nVideo exposes all of that and more.

### Mechanism

Progress is delivered via callbacks from C++ async workers using `napi_threadsafe_function`. This allows the C++ worker thread to call back into JavaScript without blocking or requiring polling.

```
C++ Async Worker Thread                    Node.js Main Thread
─────────────────────                     ────────────────────
av_read_frame loop                        
  │                                      
  ├─ every N frames ──► napi_call_threadsafe_function ──► onProgress(progress)
  │                    (zero-copy: progress is a stack-allocated struct)     
  │                                      
  ├─ on error ────────► napi_call_threadsafe_function ──► onError(err)
  │                                      
  └─ on complete ─────► napi_call_threadsafe_function ──► onComplete(result)
```

**Callbacks, not EventEmitter.** EventEmitter allocates listener arrays and wraps calls in try/catch. Callbacks are a direct function reference — zero overhead. The caller passes `onProgress`/`onComplete`/`onError` as plain function references in the options object.

**Progress struct is stack-allocated in C++.** The C++ side fills a plain struct and passes it to `napi_call_threadsafe_function`. The binding layer converts it to a plain JS object on the main thread. No heap allocation on the worker side.

### Progress by Operation

#### Transcode / Convert (File-to-File)

The richest progress info. Mirrors FFmpeg's `-progress pipe:1` output.

```javascript
nVideo.transcode(input, output, {
  video: { codec: 'libx264', crf: 23 },
  audio: { codec: 'aac', bitrate: 128000 },
  onProgress: (p) => { /* ... */ },
  onComplete: (r) => { /* ... */ }
});
```

| Field | Type | Description |
|-------|------|-------------|
| `time` | number | Current input timestamp processed (seconds) |
| `percent` | number | Progress 0-100 |
| `speed` | number | Processing speed (1.0 = realtime, 2.5 = 2.5x faster) |
| `bitrate` | number | Current output bitrate (bits/sec) |
| `size` | number | Current output file size (bytes) |
| `frames` | number | Video frames encoded so far |
| `fps` | number | Current encode throughput (frames/sec) |
| `audioFrames` | number | Audio samples encoded so far |
| `audioTime` | number | Audio timestamp processed (seconds) |
| `estimatedDuration` | number | Total input duration (from initial probe) |
| `estimatedSize` | number | Estimated final file size (bytes) |
| `eta` | number | Estimated seconds remaining |
| `dupFrames` | number | Duplicate frames dropped |
| `dropFrames` | number | Frames dropped (slow encoder) |

#### Waveform Generation

```javascript
nVideo.waveform(input, {
  points: 1000,
  onProgress: (p) => { /* ... */ }
});
```

| Field | Type | Description |
|-------|------|-------------|
| `time` | number | Current audio timestamp decoded (seconds) |
| `percent` | number | Progress 0-100 |
| `speed` | number | Decode speed multiplier |
| `eta` | number | Estimated seconds remaining |
| `estimatedDuration` | number | Total audio duration |
| `pointsComputed` | number | Waveform points calculated so far |
| `totalPoints` | number | Total waveform points requested |

#### Concat

```javascript
nVideo.concat(files, output, {
  onProgress: (p) => { /* ... */ }
});
```

| Field | Type | Description |
|-------|------|-------------|
| `fileIndex` | number | Current input file being processed |
| `totalFiles` | number | Total number of input files |
| `percent` | number | Overall progress 0-100 |
| `filePercent` | number | Progress within current file 0-100 |
| `size` | number | Current output file size (bytes) |
| `eta` | number | Estimated seconds remaining |

#### Remux

```javascript
nVideo.remux(input, output, {
  onProgress: (p) => { /* ... */ }
});
```

| Field | Type | Description |
|-------|------|-------------|
| `time` | number | Current timestamp copied (seconds) |
| `percent` | number | Progress 0-100 |
| `size` | number | Current output file size (bytes) |
| `speed` | number | Copy speed multiplier (usually very high, disk-bound) |
| `estimatedDuration` | number | Total input duration |
| `eta` | number | Estimated seconds remaining |

#### Streaming (Frame-by-Frame)

For streaming playback, progress is implicit — each `readVideoFrame()` / `readAudio()` call returns metadata with the current position.

```javascript
const frame = input.readVideoFrame(buf);
// frame.pts       — presentation timestamp (seconds)
// frame.frameNum  — frame number (monotonically increasing)
// frame.duration  — frame duration (seconds)
// frame.keyframe  — boolean, is this a keyframe?
// input.getPosition() — current position in file (seconds)
// input.getDuration() — total duration (seconds)
```

No callback needed — the caller controls the loop and has all position info from each frame.

### Progress Throttling

Progress callbacks fire at most once per ~100ms (configurable via `progressIntervalMs` in options). At 60fps video, that's roughly every 6 frames. This prevents flooding the main thread with progress updates while maintaining smooth UI updates.

```javascript
nVideo.transcode(input, output, {
  video: { codec: 'libx264', crf: 23 },
  progressIntervalMs: 50,   // default: 100
  onProgress: (p) => updateUI(p)
});
```

### Completion Result

Every async operation returns a result object on completion:

```javascript
onComplete: (result) => {
  // result.duration       — total input duration (seconds)
  // result.frames         — total video frames processed
  // result.audioFrames    — total audio samples processed
  // result.size           — final output file size (bytes)
  // result.bitrate        — average output bitrate (bits/sec)
  // result.speed          — average processing speed
  // result.timeMs         — total wall-clock time (milliseconds)
  // result.dupFrames      — total duplicate frames
  // result.dropFrames     — total dropped frames
}
```

### Error Reporting

Errors include full context — which operation failed, at what timestamp/frame, and the FFmpeg error code:

```javascript
onError: (err) => {
  // err.message     — human-readable description
  // err.code        — FFmpeg error code (e.g., AVERROR_INVALIDDATA)
  // err.operation   — which phase failed ('open', 'decode', 'encode', 'write', 'seek')
  // err.timestamp   — where in the file it failed (seconds)
  // err.frame       — which frame failed (number)
  // err.stream      — which stream failed (index)
}
```

## Build Configuration

### Prerequisites

- Visual Studio 2022 (MSVC v143) for Windows
- node-gyp v10+
- node-addon-api v8+
- FFmpeg shared libraries from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) (GPL variant)

### binding.gyp

```python
{
  'targets': [{
    'target_name': 'nvideo',
    'sources': ['src/processor.cpp', 'src/binding.cpp'],
    'include_dirs': [
      "<!@(node -p \"require('node-addon-api').include\")",
      'deps/ffmpeg/include'
    ],
    'libraries': [
      'avformat', 'avcodec', 'avutil', 'swscale', 'swresample', 'avfilter'
    ],
    'defines': ['NAPI_DISABLE_CPP_EXCEPTIONS'],
    'cflags!': ['-fno-exceptions'],
    'cflags_cc!': ['-fno-exceptions'],
    'cflags_cc': ['-std=c++17'],
    'msvs_settings': {
      'VCCLCompilerTool': { 'ExceptionHandling': 1 }
    }
  }]
}
```

### FFmpeg Libraries Required

| Library | Purpose |
|---------|---------|
| libavformat | Container demuxing/muxing |
| libavcodec | Encode/decode |
| libavutil | Core utilities |
| libswscale | Video scaling, pixel format conversion |
| libswresample | Audio resampling, format conversion |
| libavfilter | Filter graph |

## Performance Targets

| Operation | Target | Notes |
|-----------|--------|-------|
| Probe / metadata | < 10 ms | No decode, just container inspection |
| Thumbnail extraction | < 50 ms | Keyframe seek + decode single frame |
| Waveform generation | < duration + 100ms | Single-pass audio decode |
| Transcode to file | Same as ffmpeg CLI | Runs on async worker, no JS overhead |
| Remux | Disk I/O bound | Near-instant for same codec |
| Video decode (1080p H.264) | < 10 ms/frame | 100+ fps |
| Audio decode (any format) | < 5 ms/chunk | 4096 samples |
| Seek | < 20 ms | Keyframe-aligned |
| Zero-copy overhead | 0 copies | Direct write to JS memory |
| Transcode to file — JS overhead | ~0 ms | Entire pipeline in C++, Node.js is idle |
| Progress callback overhead | < 0.1 ms | Stack-allocated struct, threadsafe_function, ~100ms throttle |

## Core Development Maxims

**Priorities: Reliability > Performance > Everything else.**

**LLM-Native Codebase**: Code readability and structure for humans is a non-goal. The code will not be maintained by humans. Optimize for the most efficient structure an LLM can understand. Do not rely on conventional human coding habits.

**Vanilla JS**: No TypeScript anywhere. Code must stay as close to the bare platform as possible for easy optimization and debugging. `.d.ts` files are generated strictly for LLM/editor context, not used at runtime.

**Zero Dependencies**: If we can build it ourselves using raw standard libraries, we build it. Avoid external third-party packages. Evaluate per-case if a dependency is truly necessary.

**Fail Fast, Always**: No defensive coding. No mock data, no fallback defaults, and no silencing try/catch blocks. The goal is to write perfect, deterministic software. When it breaks, let it crash and fix the root cause.

**Pure C++ Core**: The `FFmpegProcessor` class has zero N-API dependency. The binding layer is a thin translation membrane.

**MSVC Only**: MinGW-compiled NAPI addons crash in Electron due to IAT corruption. Always build with MSVC.

## Future Considerations

- Hardware acceleration (NVENC, QSV, VAAPI, VideoToolbox) — decode side
- Multi-track mixing
- Subtitle burn-in / extraction
- HDR tone mapping
- Real-time streaming (RTMP, HLS, DASH)
- Frame-accurate editing (cut, concat, trim)
- Async workers for blocking decode operations
- Native pixel format passthrough (skip YUV→RGB when not needed)
- Stream mapping (FFmpeg `-map` equivalent)
- Pin FFmpeg version (use versioned BtbN branch, not `master-latest`)
