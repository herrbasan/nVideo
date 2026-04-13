# nVideo - Development Plan

**Last Updated**: 2026-04-13 (Video concat timestamp fix)

**Spec**: [nVideo_spec.md](nVideo_spec.md)

**Reference Projects** (in `_Reference/`):
- **nImage** (`_Reference/nImage/`) — N-API binding patterns, Electron compatibility, binary loading, build system (MSVC + vcpkg), AGENTS.md structure. Key files: `src/binding.cpp`, `src/decoder.h/.cpp`, `lib/index.js`, `binding.gyp`, `AGENTS.md`
- **ffmpeg-napi-interface** (`_Reference/ffmpeg-napi-interface/`) — FFmpeg C API integration, audio decode, zero-copy Float32Array, SAB ring buffer, AudioWorklet streaming, AsyncWorker progress, three-phase EOF drain, FFmpeg 7.0+ AVChannelLayout API. Key files: `src/decoder.h/.cpp`, `src/binding.cpp`, `lib/player-sab.js`, `lib/ffmpeg-worklet-sab.js`, `scripts/download-ffmpeg.js`

**Electron Compatibility** (critical, non-negotiable):
- **MSVC builds only** — MinGW-compiled NAPI addons crash in Electron due to empty Import Address Tables for `napi_*` functions (0xC0000005 Access Violation). Proven in nImage. Always use `node-gyp` with MSVC toolset.
- **Electron rebuild** — `npx electron-rebuild -f -w nvideo -v <version>` after building for Node.js
- **DLL loading** — FFmpeg DLLs must be in the same directory as the `.node` file, or on the system PATH. Use `binding.gyp` copies to `build/Release/`.
- **Binary loading order** — project `bin/` (reliable, version-controlled) → `build/Release/` (dev) → `dist/` (distribution). See nImage `lib/index.js` for the 4-location fallback pattern.
- **Renderer process** — `nodeIntegration: true` required, or use IPC from main process. Test both.
- **Worklet reuse** — reuse `AudioWorkletNode` instances across track switches to avoid Chrome memory leak (~8-10MB per 30 switches). Proven in ffmpeg-napi-interface.
- **SAB headers** — `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` required for `SharedArrayBuffer` in renderer.

**Test Assets** (real-world files from `D:\Work\_GIT\MediaService\tests\assets\`):

| File | Format | Duration | Size | Streams |
|------|--------|----------|------|---------|
| `videos/2019_05_06_Republica.mp4` | MP4 | 119.9s | 721MB | H.264 4K 30fps + AAC stereo |
| `videos/IMG_0104.MOV` | MOV | 8.6s | 50MB | HEVC 4K 60fps + AAC stereo |
| `videos/Tanz in den Mai_bas.mp4` | MP4 | 271.8s | 2327MB | H.264 4K 60fps + AAC stereo |
| `audio/Bär.m4a` | M4A | 256.1s | 3MB | AAC stereo |
| `audio/Stream_1.mp3` | MP3 | 60.0s | 1MB | MP3 stereo |
| `audio/Vangengel.wav` | WAV | 82.0s | 14MB | PCM 16-bit stereo |
| `audio/healme.flac` | FLAC | 21.3s | 1MB | FLAC stereo |
| `audio/mdjam_step2.ogg` | OGG | 340.2s | 5MB | Vorbis stereo |

**Test Output**: `test_media/test_output/` — temporary output files from ad-hoc tests (gitignored).

---

## Architecture: Two Pipelines

nVideo has two fundamentally different pipelines:

### Pipeline A: Transcode (File → File) — PRIMARY

Maximum performance file-to-file transcoding. Equivalent to `ffmpeg` CLI but with better progress, hardware acceleration, and caching.

**Operations**: `probe()`, `thumbnail()`, `waveform()`, `transcode()`, `remux()`, `convert()`, `concat()`, `extractStream()`, `extractAudio()`

**Design**: Input file path → C++ pipeline → output file path. Zero JS involvement during processing.

### Pipeline B: Streaming (Chunk → Chunk) — FUTURE

Frame-by-frame decode into caller-owned buffers. Primary use case: TTS/STT services.

**Operations**: `openInput()`, `readAudio()`, `readVideoFrame()`, `seek()`, `close()`

**Design**: Caller controls decode loop, C++ writes directly into caller's buffers (zero-copy).

---

## Pipeline A: Transcode Phases

### Phase A1: Core Utilities ✅ COMPLETE

Probe, thumbnail, waveform — the fastest operations, already working.

- [x] `probe()` — 49x faster than ffprobe (1.28ms vs 62.48ms)
- [x] `thumbnail()` — 15x faster than ffmpeg CLI for SD/HD
- [x] `waveform()` — 146ms for 340s audio file

### Phase A2: Transcode Foundation ✅ COMPLETE

Basic transcode, remux, convert working. CRF/preset and benchmark done.

- [x] `transcode()` — full re-encode pipeline, C++ only
- [x] `remux()` — stream copy, near-instant
- [x] `convert()` — shorthand with auto-defaults
- [x] Video filter graphs (scale verified working)
- [x] Audio filter graphs (abuffer → aformat → abuffersink)
- [x] Progress callbacks (onProgress, onComplete, onError)
- [x] Completion result + error context
- [x] **CRF/preset fixed** — `av_opt_set` works, just needed `#include <libavutil/opt.h>` (2026-04-13)
- [x] **Benchmark vs CLI** — nVideo 2914ms vs ffmpeg CLI 3078ms for OGG→MP3 320k (340s file). nVideo is 1.06x — parity confirmed (2026-04-13)

### Phase A3: Audio Extraction ✅ COMPLETE

Dedicated function to extract audio from video files (decode + re-encode).

- [x] Implement `FFmpegProcessor::extractAudio(inputPath, outputPath, opts)` in C++
  - Full audio decode from video container
  - Re-encode to target format (WAV, MP3, AAC, FLAC, Opus)
  - Video stream discarded (`-vn` equivalent)
  - Progress callbacks, completion result
  - Auto-detect codec from output extension (`.wav` → pcm_s16le, `.mp3` → libmp3lame, etc.)
  - Uses same filter graph pipeline as transcode (abuffer → aformat → asetnsamples → abuffersink)
  - Handles PCM encoders with `frame_size=0` (skips asetnsamples filter)
- [x] Add NAPI binding: `nVideo.extractAudio(input, output, opts)`
- [x] JS convenience wrapper in `lib/index.js`
- [x] Tests: extract audio from MP4 → WAV (18ms), MP4 → MP3 (23ms) (2026-04-13)

### Phase A4: Caching System ✅ COMPLETE

Hash-based cache to avoid redundant transcoding.

- [x] Design cache key: `SHA256(input_path + input_mtime + JSON.stringify(config))`
- [x] Implement cache lookup/store in JS layer (lib/index.js)
- [x] Cache directory: `.nvideo-cache/` (configurable via `cacheDir`)
- [x] Cache metadata: `cache.json` with entry info
- [x] API: `cache: true/false`, `cacheDir`, `onCacheHit`, `onCacheMiss`
- [x] `nVideo.clearCache()` — remove all or by age
- [x] Integrated into `transcode()`, `remux()`, `convert()`, `extractAudio()`
- [x] Tests: cache hit returns instantly (4ms vs 2960ms), cache miss transcodes, clearCache works (2026-04-13)

### Phase A5: Hardware Acceleration ✅ COMPLETE

GPU-accelerated encode/decode via FFmpeg's hardware encoders.

- [x] Support `hwaccel` option: `'cuda'`, `'qsv'`, `'vaapi'`, `'d3d11va'`
- [x] Hardware decode setup with `av_hwdevice_ctx_create`
- [x] Hardware encoder support with `hw_frames_ctx`
- [x] HW pixel format detection and selection
- [x] JS API: `hwaccel: 'cuda'` in transcode options
- [x] Binding: parse `hwaccel` from JS options
- [x] Tested: NVENC (h264_nvenc) working - 1187ms vs 14880ms software (12.5x faster)
- [x] QSV partially works (depends on Intel GPU driver support) (2026-04-13)

### Phase A6: Transcode Polish ⬜

Fix remaining issues, optimize performance.

- [x] Fix concat timestamp handling - time_base rescaling fixed (av_rescale_q)
- [x] Rewrite concat to use FFmpeg concat demuxer (file list approach)
- [x] Audio concat working (MP3 tested)
- [x] Video concat timestamp issues - FIXED (2026-04-13)
  - Replaced concat demuxer with manual per-file processing
  - Cumulative DTS offset tracking per stream
  - Handles negative start times (edit lists in MOV/MP4)
  - Dynamic offset adjustment when PTS < DTS would occur
  - Uses av_write_frame instead of av_interleaved_write_frame
  - Tested: 2x 8.57s HEVC files → 17.63s output (within 3% of expected 17.13s)
- [x] Fix remux progress stats - now uses packet timestamps instead of wall clock time
- [ ] Profile hot paths: decode, filter, encode loops
- [ ] Optimize filter graph setup (avoid redundant reinitialization)

---

## Pipeline B: Streaming Phases

### Phase B1: Core Decode API ✅ COMPLETE

Frame-by-frame decode into caller-owned buffers.

- [x] `openInput(path)` — open file, returns decoder instance
- [x] `readAudio(numSamples)` — zero-copy into Float32Array (0.18ms/chunk)
- [x] `readVideoFrame(buffer)` — zero-copy into Uint8Array (~30fps 4K)
- [x] `seek(seconds)` — jump to timestamp
- [x] `close()` — release resources
- [x] Multi-format support: MP3, FLAC, WAV, AAC, OGG, H.264, HEVC, VP9

### Phase B2: Streaming Players ⬜ FUTURE

AudioWorklet and VideoFrame players for real-time playback.

- [ ] `lib/player-audio.js` — SAB ring buffer + AudioWorklet
- [ ] `lib/player-video.js` — SAB frame queue + VideoFrame/Canvas
- [ ] Gapless looping, pause/resume, position reporting
- [ ] Synchronized A/V playback
- [ ] Tests: Electron renderer, COOP/COEP headers

### Phase B3: Buffer Pool ⬜ FUTURE

Pre-allocated buffers for zero-GC streaming loops.

- [ ] `nVideo.createBufferPool(opts)` — acquire/release pattern
- [ ] Integrate into streaming decode loop
- [ ] Benchmark: GC pressure with vs without pool

---

## Known Issues

| Issue | Impact | Priority |
|-------|--------|----------|
| Audio concat duration metadata | Output shows single file duration instead of total | Low (playback works, metadata wrong) |
| Concat mixed formats | Can't concat different audio formats (e.g., FLAC + MP3 → MP3) | Medium |
| HW decode + SW encode | "bad dst image pointers" warnings when using hwaccel with software encoder | Medium |
| FFmpeg crashes on corrupt files | Error handling can't catch | Low (FFmpeg limitation) |

## Success Criteria

| Criterion | Target | Status |
|-----------|--------|--------|
| `probe()` latency | < 10 ms | ✅ 1.28ms |
| `thumbnail()` latency | < 50 ms (SD/HD) | ✅ 4.55ms |
| Audio decode (any format) | < 5 ms/chunk | ✅ 0.18ms |
| Video decode (1080p H.264) | < 10 ms/frame | ⚠️ ~33ms (4K) |
| Transcode speed | Matches `ffmpeg` CLI (within 5%) | ✅ 1.06x (OGG→MP3, 340s) |
| Transcode progress | Fires every ~100ms, accurate ETA | ✅ Working |
| Zero-copy verified | No intermediate copies in decode path | ✅ Verified |
| Electron compatible | MSVC build, no crashes in renderer | ✅ Verified |
| Concat video | Matches expected duration (within 5%) | ✅ 17.63s vs 17.13s (2.9% diff) |
| Memory | No leaks in 1-hour streaming loop | ⬜ Not tested |
