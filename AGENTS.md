# nVideo - Agent Development Guide

**Project**: Native video/audio processing for Node.js via N-API + FFmpeg
**Spec**: [docs/nVideo_spec.md](docs/nVideo_spec.md)
**Dev Plan**: [docs/nVideo_dev_plan.md](docs/nVideo_dev_plan.md)

## Vision

nVideo is a native Node.js module (N-API) that brings FFmpeg's full media processing capabilities to JavaScript. It is the video/media counterpart to nImage — where nImage makes exotic image formats accessible through a native API, nVideo makes professional-grade video and audio processing accessible through a native FFmpeg interface.

The N-API addon exists because **streaming decoded data into JavaScript memory** is impossible with CLI wrappers. Zero-copy into `SharedArrayBuffer` for AudioWorklet/VideoFrame consumption in Electron is the core differentiator. But the utility functions (probe, thumbnail, waveform, transcode) are where 90% of usage happens and justify nVideo over spawning `ffmpeg.exe`.

---

## Design Principles

1. **Zero-copy by default** — decoded data is written directly into JS-owned memory. No intermediate copies between FFmpeg and JavaScript.
2. **File output bypasses JS entirely** — when transcoding to file, FFmpeg's native I/O writes directly to disk. The entire read → decode → filter → encode → write pipeline runs in C++ memory. Node.js only initiates the operation and receives a completion callback. No decoded frames ever enter V8 heap.
3. **Pure C++ core, thin N-API wrapper** — the `FFmpegProcessor` class has zero N-API dependencies. The binding layer is a thin translation membrane.
4. **FFmpeg-native API** — the JS API maps 1:1 to FFmpeg's C API and CLI. Existing FFmpeg recipes translate directly. Filter graphs use FFmpeg's filter graph syntax. Codec options map to `AVOptions`.
5. **Performance-first JS surface** — no method chaining, no intermediate objects. The API uses flat function calls and configuration objects. A thin JS convenience layer may wrap the FFmpeg-native core, but it must not introduce allocation overhead.
6. **No CLI dependency** — links directly against FFmpeg's C libraries (`libavformat`, `libavcodec`, `libswscale`, `libswresample`, `libavfilter`). No `ffmpeg.exe` needed.
7. **Rich progress reporting** — every long-running operation reports detailed, real-time progress back to JavaScript. Progress data mirrors what FFmpeg's `-progress` flag provides: timestamps, frame counts, bitrates, speed, ETA. Progress is delivered via callbacks from async workers (`napi_threadsafe_function`), never EventEmitter.
8. **Electron-compatible** — MSVC-built binaries, proper DLL loading, tested in renderer and main process.
9. **Checkpoint before debugging** — when you encounter problems, always make a commit before starting to change things, so you have a clean state to return to when you actually figure out the cause/solution.
10. **When in doubt, ask the Human** — otherwise, this is your project. Be as smart and creative as possible, and have fun!

---

## Core Development Maxims

**Priorities: Reliability > Performance > Everything else.**

**LLM-Native Codebase**: Code readability and structure for humans is a non-goal. The code will not be maintained by humans. Optimize for the most efficient structure an LLM can understand. Do not rely on conventional human coding habits.

**Vanilla JS**: No TypeScript anywhere. Code must stay as close to the bare platform as possible for easy optimization and debugging. `.d.ts` files are generated strictly for LLM/editor context, not used at runtime.

**Zero Dependencies**: If we can build it ourselves using raw standard libraries, we build it. Avoid external third-party packages. Evaluate per-case if a dependency is truly necessary.

**Fail Fast, Always**: No defensive coding. No mock data, no fallback defaults, and no silencing try/catch blocks. The goal is to write perfect, deterministic software. When it breaks, let it crash and fix the root cause.

**Pure C++ Core**: The `FFmpegProcessor` class has zero N-API dependency. The binding layer is a thin translation membrane.

**MSVC Only**: MinGW-compiled NAPI addons crash in Electron due to IAT corruption. Always build with MSVC.

---

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

### Two Data Paths

**Path A — Transcode to File (90% of usage):** Entire pipeline in C++ on async worker thread. FFmpeg reads disk → decodes → filters → encodes → writes disk. Zero frames touch V8 heap. Node.js gets a callback when done.

**Path B — Stream to JavaScript (real-time playback):** Frame-by-frame decode, zero-copy into caller's `Uint8Array`/`Float32Array` or `SharedArrayBuffer`. For Electron AudioWorklet / VideoFrame consumption.

### Project Structure

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
│   ├── player-audio.js      # Audio streaming player (SAB + AudioWorklet)
│   └── buffer-pool.js       # BufferPool, RingBuffer, AVStreamPlayer
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
└── package.json
```

---

## Reference Projects

| Project | Location | What to learn from |
|---------|----------|--------------------|
| **nImage** | `_Reference/nImage/` | N-API binding patterns, Electron compatibility, binary loading, build system (MSVC + vcpkg), AGENTS.md structure |
| **ffmpeg-napi-interface** | `_Reference/ffmpeg-napi-interface/` | FFmpeg C API integration, audio decode, zero-copy Float32Array, SAB ring buffer, AudioWorklet streaming, AsyncWorker progress, three-phase EOF drain, FFmpeg 7.0+ AVChannelLayout API |

---

## Electron Compatibility

- **MSVC builds only** — MinGW-compiled NAPI addons crash in Electron (IAT corruption, 0xC0000005). Proven in nImage.
- **Electron rebuild** — `npx electron-rebuild -f -w nvideo -v <version>` after building for Node.js
- **DLL loading** — FFmpeg DLLs must be in same directory as `.node` file or on system PATH
- **Binary loading order** — project `bin/` → `build/Release/` → `dist/` (see nImage 4-location fallback)
- **SAB headers** — `Cross-Origin-Opener-Policy: same-origin` + `Cross-Origin-Embedder-Policy: require-corp` required for SharedArrayBuffer in renderer
- **Worklet reuse** — reuse AudioWorkletNode instances across track switches to avoid Chrome memory leak (~8-10MB per 30 switches)

---

## Development Phases

| Phase | Status | Description |
|-------|--------|-------------|
| A1 | ✅ | Core Utilities: probe, thumbnail, waveform |
| A2 | ✅ | Transcode Foundation: transcode, remux, convert, filters, progress |
| A3 | ✅ | Audio Extraction: extractAudio |
| A4 | ✅ | Caching System: SHA256-based, transmit-once TTL |
| A5 | ✅ | Hardware Acceleration: NVENC, QSV, VAAPI, D3D11VA |
| A6 | ✅ | Transcode Polish: concat fix, remux stats, profiling |
| B1 | ✅ | Core Decode API: openInput, readAudio, readVideoFrame, seek, close |
| B2 | ✅ | Streaming Players: AudioWorklet + VideoFrame players |
| B3 | ✅ | Buffer Pool: pre-allocated buffers, zero GC pressure |
| 10 | ⬜ | Advanced: complex filters, stream mapping, HDR, network |

---

## Performance Targets

| Operation | Target |
|-----------|--------|
| Probe / metadata | < 10 ms |
| Thumbnail extraction | < 50 ms |
| Waveform generation | < duration + 100ms |
| Transcode to file | Same as ffmpeg CLI |
| Remux | Disk I/O bound |
| Video decode (1080p H.264) | < 10 ms/frame (100+ fps) |
| Audio decode | < 5 ms/chunk (4096 samples) |
| Seek | < 20 ms |
| Zero-copy overhead | 0 copies |
| Transcode JS overhead | ~0 ms |
| Progress callback overhead | < 0.1 ms |

---

## Agent Notes

_This section is for the agent to record discoveries, patterns, gotchas, and decisions during development._

<!-- Add notes below as you learn things -->

### Phase 0 Discoveries (2026-04-12)

- **FFmpeg DLL versions**: BtbN latest (as of 2026-04-12) ships avformat-62, avcodec-62, avutil-60, swscale-9, swresample-6, avfilter-11. These are hardcoded in `binding.gyp` copies section — will need updating when FFmpeg major versions bump.
- **MSVC /std:c++17 vs /std:c++20**: node-gyp sets `/std:c++20` by default, our binding.gyp overrides to `/std:c++17`. This produces a benign D9025 warning. No issue.
- **`npm install` runs `node-gyp rebuild`**: Removed explicit `install` script from package.json. Use `npm install --ignore-scripts` then `npm run build` manually to control FFmpeg download timing.
- **Binary loading order**: `bin/` (project-level, Electron-reliable) → `build/Release/` (dev) → `dist/` (distribution). Three locations, not four — nImage had prebuilds/ which we don't use.

### Phase 0 Verification (2026-04-12) ✅ COMPLETE

**MSVC Build Verified:**
- VS2022 (v143 toolset) successfully builds `nvideo.node` (126,464 bytes)
- 6 FFmpeg DLLs copied to `build/Release/`: avformat-62.dll, avcodec-62.dll, avutil-60.dll, swscale-9.dll, swresample-6.dll, avfilter-11.dll
- `npm run build` works: FFmpeg download skipped (already present), configure + build succeed

**JS Loading Verified:**
- `require('./lib')` loads successfully
- `n.version()` returns `"0.1.0"`
- Only export is `version` (as expected for Phase 0 skeleton)

**Electron Rebuild Verified:**
- `@electron/rebuild -f -w nvideo` succeeds with Electron v24.14.0
- Rebuilt `nvideo.node` loads correctly in Electron
- Use `electron@latest` (not pinned version) — old Electron headers (v16) no longer available at standard URL

### Phase B2 Implementation (2026-04-14) ✅ COMPLETE

**Streaming Players:**
- `lib/player-audio.js` — AudioWorklet player with SAB ring buffer (510 lines)
  - AudioWorklet processor embedded as blob URL (no external file needed)
  - Control buffer: 12 Int32 slots (writePtr, readPtr, state, sampleRate, channels, loop, totalFrames, underruns, startTime)
  - Two-part ring buffer write for wrap-around handling
  - Drift-corrected setTimeout feed loop (20ms interval)
  - Worklet reuse across track switches (SAB preserved when sample rate matches)
  - CPU optimization: disconnect worklet on pause
  - Gapless looping via decoder seek(0) on EOF
- `lib/player-video.js` — VideoFrame player with canvas rendering (370 lines)
  - Frame queue + canvas 2D rendering
  - SAB ring buffer for decoded RGB frames
  - Separate feed loop and render loop (feed at 16ms, render at FPS)
  - Position tracking via frame number extrapolation
- Both players exported via `nVideo.AudioStreamPlayer` and `nVideo.VideoStreamPlayer`

**Performance verified:**
- Audio decode: 60s MP3 in 39ms (1538x realtime)
- Video decode: 4K H.264 at 31.6 fps (31.69ms/frame)

### Phase A2 Insights (2026-04-16)

- **The `av_frame_make_writable` Performance Trap**: While `av_frame_make_writable(scaledFrame)` safely ensures buffer ownership before `sws_scale`, it comes with a hidden cost: if the encoder still holds a lock on the buffer from the previous frame, `av_frame_make_writable` explicitly performs a deep `memcpy` of the *entire 1080p/4K pixel payload* into the new buffer to "preserve" existing data you're about to overwrite anyway. To avoid wasting massive RAM bandwidth during scaling, it's significantly faster to manually `av_frame_unref(scaledFrame)`, re-apply `format`/`width`/`height` properties, and call `av_frame_get_buffer(scaledFrame, 32)` to allocate a fresh, uncopied buffer.
- **Audio FIFO replacement via Node Graphs**: The AAC encoder's infamous `Timestamps are unset in a packet` and `Could not update timestamps for skipped samples` strict errors do NOT require you to manually write a robust `AVAudioFifo` First-In-First-Out queue in C++. The `asetnsamples` audio filter graph node handles this transparently, buffering input arrays and predictably chunking them exclusively to the encoder's `frame_size` limit dynamically.
- **Multiplex Interleave Mathematics**: While the CLI does magical synchronization buffering implicitly, the native C API demands explicit TS mapping. Immediately before calling `av_interleaved_write_frame()`, encoded output packet timebases must be forcefully translated out of the `encoder->time_base` coordinate grid into the destination stream's `outStream->time_base` scale via `av_packet_rescale_ts()`. Otherwise, the muxer will error or stall entirely thinking frames are drifting into the future or past sequentially.
- **Memory Leak Stress Testing**: To guarantee zero-leak C++ implementations without V8 masking allocations, the `--expose-gc` flag allows manual `global.gc()` sweeps inside JavaScript `for` loops. By monitoring `process.memoryUsage().external`, an iterative 10-pass multiplexing transcode stress-test confirmed that memory usage perfectly stabilizes (flat ~97MB RSS, ~5.2MB External) after the initial internal buffer pool allocations. This definitively proves the `av_frame_unref()` lifecycle and custom Node graphs leak zero bytes round-over-round.
- **AVFrame Pre-Allocation Optimization**: Avoid invoking `av_frame_alloc()` and `av_frame_free()` inside the hot-path `while (av_read_frame)` loop. While allocating just the frame struct is relatively cheap, doing so hundreds of times per second (especially for video and audio filters concurrent processing) generates C++ heap fragmentation and jitter. Hoist `swVideoFrame`, `videoFilteredFrame`, and `audioFilteredFrame` out of the loop and reuse them with `av_frame_unref()` to ensure steady, fast performance.
- **Rich Transcode Telemetry**: In addition to standard percentage completion, the native binding now provides highly detailed real-time data back to JavaScript. This includes live tracking of `size` (bytes written), `bitrate` calculations mid-stream, `estimatedSize` projections, isolated `audioTime` measurements (to diagnose A/V desync conditions), and discrete `dupFrames`/`dropFrames` counters every 100ms.
