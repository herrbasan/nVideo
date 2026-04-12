# nVideo - Development Plan

**Last Updated**: 2026-04-12

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

---

## Phase 0: Project Scaffolding

- [x] Initialize npm project (`package.json`, node-gyp, node-addon-api)
- [x] Create `binding.gyp` with FFmpeg library linkage (spec: Build Configuration)
- [x] Create `src/processor.h` — FFmpegProcessor class skeleton, zero N-API dependency
- [x] Create `src/processor.cpp` — empty implementation
- [x] Create `src/binding.cpp` — minimal NAPI module registration, exports `version`
- [x] Create `lib/index.js` — native binary loading (3-location fallback: bin/ → build/Release → dist/)
- [x] Create `scripts/download-ffmpeg.js` — download FFmpeg shared libs from BtbN (GPL shared, auto-platform)
- [x] Create `AGENTS.md` — LLM development guide (vision, principles, maxims, architecture, phases)
- [x] Verify: `node -e "require('./lib')"` loads and `version()` returns `"0.1.0"`
- [x] Verify: MSVC build produces `nvideo.node` + 6 FFmpeg DLLs in `build/Release/`
- [ ] Verify: `npx electron-rebuild -f -w nvideo -v <version>` produces Electron-compatible binary

## Phase 1: Probe / Metadata

Reference: spec → Utility Functions → `probe()`, API ↔ FFmpeg C API Mapping

- [ ] Implement `FFmpegProcessor::probe(path)` in C++
  - `avformat_open_input` → `avformat_find_stream_info` → extract stream info
  - Return: format info, stream array (codec, dimensions, sample rate, bitrate, duration), tags
- [ ] Implement `FFmpegProcessor::getFileMetadata(path)` — static method, no full processor needed
- [ ] Add NAPI binding: `nVideo.probe(path)` → plain JS object
- [ ] Add NAPI binding: `nVideo.getMetadata(path)` → plain JS object
- [ ] JS convenience: `lib/index.js` wraps native binding
- [ ] Tests: probe MP4, MKV, WAV, MP3 — verify all fields populated
- [ ] Tests: probe corrupt file — verify error with context (spec: Error Reporting)
- [ ] Benchmark: probe latency vs `ffprobe` CLI spawn

## Phase 2: Thumbnail Extraction

Reference: spec → Utility Functions → `thumbnail()`

- [ ] Implement `FFmpegProcessor::thumbnail(path, timestamp, width)` in C++
  - Seek to nearest keyframe (`av_seek_frame` with `AVSEEK_FLAG_BACKWARD`)
  - Decode single video frame (`avcodec_send_packet` → `avcodec_receive_frame`)
  - Scale via `sws_scale` to target width (maintain aspect ratio)
  - Convert to RGB24
- [ ] Add NAPI binding: `nVideo.thumbnail(path, opts)` → `{ data: Uint8Array, width, height, format }`
  - Output writes directly into `Napi::Uint8Array` (zero-copy)
- [ ] Tests: thumbnail from MP4 at various timestamps
- [ ] Tests: thumbnail from MKV, AVI, MOV
- [ ] Tests: thumbnail at timestamp 0 (first frame)
- [ ] Benchmark: thumbnail latency

## Phase 3: Audio Decode (Zero-Copy)

Reference: spec → Zero-Copy Strategy → Audio, Core API → `readAudio()`

- [ ] Implement `FFmpegProcessor::open(path)` — open input, find audio stream, init decoder, init `SwrContext`
  - Use FFmpeg 7.0+ `AVChannelLayout` API (`swr_alloc_set_opts2`) — see ffmpeg-napi-interface `src/decoder.cpp:129`
  - Output: interleaved float32 stereo at configurable sample rate
  - Multi-threaded decode: `FF_THREAD_FRAME | FF_THREAD_SLICE`
- [ ] Implement `FFmpegProcessor::readAudio(float* outBuffer, int numSamples)` — decode + resample into caller's buffer
  - Internal sample buffer (1 second), reuse across calls — see ffmpeg-napi-interface `src/decoder.cpp:decodeNextFrame()`
  - Three-phase EOF drain: decoder → resampler → signal EOF — see ffmpeg-napi-interface `src/decoder.cpp`
- [ ] Implement `FFmpegProcessor::seek(seconds)` — `av_seek_frame`, flush codec, reset resampler — see ffmpeg-napi-interface `src/decoder.cpp:seek()` (swr_close + swr_init pattern)
- [ ] Implement `FFmpegProcessor::close()` — clean up all FFmpeg contexts
- [ ] Add NAPI bindings:
  - `nVideo.openInput(path)` → input handle
  - `input.openDecoder(streamIndex)`
  - `input.readAudio(numSamples)` → `Float32Array` (zero-copy via `Napi::Float32Array::New`) — see ffmpeg-napi-interface `src/binding.cpp:Read()`
  - `input.seek(seconds)`
  - `input.getDuration()`, `input.getSampleRate()`, `input.getChannels()`, `input.getTotalSamples()`
  - `input.close()`
- [ ] Tests: decode MP3, FLAC, WAV, AAC, OGG — verify float32 range, non-silence
- [ ] Tests: seek accuracy — seek to 50%, read, verify non-silence
- [ ] Tests: EOF handling — read past end returns 0 samples
- [ ] Tests: multiple concurrent decoder instances
- [ ] Benchmark: decode throughput vs ffmpeg CLI

## Phase 4: Video Decode (Zero-Copy)

Reference: spec → Zero-Copy Strategy → Video, Core API → `readVideoFrame()`

- [ ] Implement `FFmpegProcessor::readVideoFrame(uint8_t* outBuffer, int bufSize, ...)` in C++
  - Decode video frame via `avcodec_send_packet` → `avcodec_receive_frame`
  - Scale/convert via `sws_scale` to target pixel format (default: RGB24)
  - Write directly into caller's buffer (zero-copy)
  - Return actual dimensions, pixel format, PTS, frame number, keyframe flag
- [ ] Support native pixel format passthrough (skip `sws_scale` when caller wants YUV420P)
- [ ] Add NAPI bindings:
  - `input.readVideoFrame(buffer)` → frame info object
  - `input.getPosition()` — current timestamp
  - Frame metadata: `pts`, `frameNum`, `duration`, `keyframe`, `width`, `height`, `format`
- [ ] Tests: decode H.264 MP4, H.265 MKV, VP9 WebM — verify buffer populated, dimensions correct
- [ ] Tests: keyframe detection — verify `keyframe` flag
- [ ] Tests: native format passthrough — read YUV420P without RGB conversion
- [ ] Benchmark: 1080p H.264 decode fps

## Phase 5: Waveform Generation

Reference: spec → Utility Functions → `waveform()`, Progress Reporting → Waveform

- [ ] Implement `FFmpegProcessor::waveform(path, numPoints, callback)` in C++
  - Full audio decode pass, compute per-point peak amplitudes for L/R channels
  - Callback fires every ~100ms with progress (time, percent, speed, eta) — see ffmpeg-napi-interface `src/decoder.cpp:getWaveformStreaming()` for callback pattern
- [ ] Add NAPI binding: `nVideo.waveform(path, opts)` — sync (returns result) and async (with `onProgress`)
  - Async variant uses `Napi::AsyncWorker` + `napi_threadsafe_function` for progress callbacks
- [ ] Return: `{ peaksL: Float32Array, peaksR: Float32Array, duration }`
- [ ] Tests: waveform from MP3, FLAC — verify peak values, duration match
- [ ] Tests: progress callback fires, percent reaches 100
- [ ] Benchmark: waveform generation time vs duration

## Phase 6: Transcode to File

Reference: spec → Transcode to File, Two Data Paths → Path A, Progress Reporting → Transcode

- [ ] Implement `FFmpegProcessor::transcode(inputPath, outputPath, opts, progressCb)` in C++
  - Full pipeline: `avformat_open_input` → `av_read_frame` → `avcodec_decode` → `avfilter` → `avcodec_encode` → `av_interleaved_write_frame` → `av_write_trailer`
  - Entire pipeline in C++ memory — no frames cross N-API boundary
  - Progress callback via `napi_threadsafe_function` (spec: Progress Reporting → Mechanism)
  - Progress struct: time, percent, speed, bitrate, size, frames, fps, eta, dup/drop frames
  - Run on `Napi::AsyncWorker` background thread — see ffmpeg-napi-interface for JS→C++ callback pattern
- [ ] Support video codec options: codec, width, height, crf, preset, pixelFormat, bitrate
- [ ] Support audio codec options: codec, bitrate, sampleRate
- [ ] Support video/audio filter graphs (FFmpeg native syntax)
- [ ] Add NAPI bindings:
  - `nVideo.transcode(input, output, opts)` — sync variant
  - Async variant with `onProgress`, `onComplete`, `onError` callbacks
  - `nVideo.remux(input, output, opts)` — stream copy (no re-encode)
  - `nVideo.convert(input, output, opts)` — shorthand with auto-detected defaults
- [ ] Completion result: duration, frames, audioFrames, size, bitrate, speed, timeMs
- [ ] Error context: operation, timestamp, frame, stream, FFmpeg error code
- [ ] Tests: transcode MKV→MP4 (H.264 + AAC)
- [ ] Tests: transcode with video filter (scale=1280:720)
- [ ] Tests: remux (stream copy, near-instant)
- [ ] Tests: progress callback fires, percent reaches 100, completion result accurate
- [ ] Tests: error on corrupt input — verify error context
- [ ] Benchmark: transcode speed vs `ffmpeg` CLI

## Phase 7: Convenience Functions

Reference: spec → More Convenience Functions, JS Convenience Layer

- [ ] Implement `nVideo.concat(files, output, opts)` in C++ — join multiple files
  - Progress includes `fileIndex`, `totalFiles`, `filePercent`
- [ ] Implement `nVideo.extractStream(input, output, opts)` — extract single stream
- [ ] JS layer: all convenience functions in `lib/index.js`
- [ ] Tests: concat 3 MP4 files
- [ ] Tests: extract audio track to AAC

## Phase 8: Streaming (SAB + AudioWorklet)

Reference: spec → Streaming, Zero-Copy Strategy → SharedArrayBuffer, Phase 3 (Feature Scope)

- [ ] Implement `lib/player-audio.js` — SAB ring buffer player — port from ffmpeg-napi-interface `lib/player-sab.js`
  - Fixed-size `SharedArrayBuffer` for control + audio ring buffer
  - `Atomics` for synchronization between main thread and AudioWorklet
  - `FFmpegSABProcessor` AudioWorklet — reads from ring buffer, outputs to speakers — see ffmpeg-napi-interface `lib/ffmpeg-worklet-sab.js`
  - Worklet node reuse strategy (avoid Chrome memory leak) — see ffmpeg-napi-interface `lib/player-sab.js`
- [ ] Implement `lib/player-video.js` — SAB video frame queue
  - Write decoded frames into SAB, consumed by renderer via VideoFrame API / Canvas
- [ ] Audio: gapless looping, pause/resume, position reporting
- [ ] Video: frame queue with backpressure, keyframe-only seeking for responsiveness
- [ ] Tests: audio streaming — decode MP3, play through AudioWorklet, verify output
- [ ] Tests: video streaming — decode MP4, render frames, verify no drops at 30fps
- [ ] Tests: SAB memory layout — verify no corruption under concurrent read/write
- [ ] Tests: Electron renderer — verify `.node` loads, SAB works with COOP/COEP headers
- [ ] Benchmark: streaming latency (frame decode → renderer display)

## Phase 9: Buffer Pool + Optimization

Reference: spec → Future Considerations

- [ ] Implement `nVideo.createBufferPool(opts)` — pre-allocated frame buffers
  - `acquire()` → returns buffer from pool (no allocation)
  - `release(buffer)` → returns buffer to pool
- [ ] Integrate buffer pool into streaming decode loop
- [ ] Benchmark: GC pressure with vs without buffer pool in tight render loop
- [ ] Profile and optimize hot paths: decode, sws_scale, swr_convert
- [ ] Add `readInto()` variant for audio — write into pre-allocated Float32Array, return sample count

## Phase 10: Advanced Features

Reference: spec → Phase 4 (Feature Scope), Future Considerations

- [ ] Complex filter graphs (`-filter_complex` equivalent)
- [ ] Stream mapping (FFmpeg `-map` equivalent)
- [ ] Manual output control: `openOutput()`, `addStream()`, `writeHeader()`, `writePacket()`, `writeTrailer()`
- [ ] Subtitle extraction / burn-in
- [ ] Hardware acceleration (NVENC, QSV, VAAPI, VideoToolbox)
- [ ] HDR tone mapping
- [ ] Network streaming output (RTMP, HLS, DASH)
- [ ] Pin FFmpeg version in download script

## Success Criteria

| Criterion | Target |
|-----------|--------|
| `probe()` latency | < 10 ms |
| `thumbnail()` latency | < 50 ms |
| Audio decode (any format) | < 5 ms/chunk |
| Video decode (1080p H.264) | < 10 ms/frame (100+ fps) |
| Transcode speed | Matches `ffmpeg` CLI (within 5%) |
| Transcode progress | Fires every ~100ms, accurate ETA |
| Zero-copy verified | No intermediate copies in decode path |
| Electron compatible | MSVC build, no crashes in renderer, SAB works with COOP/COEP |
| Electron rebuild | `npx electron-rebuild -f -w nvideo` produces working binary |
| Memory | No leaks in 1-hour streaming loop |
