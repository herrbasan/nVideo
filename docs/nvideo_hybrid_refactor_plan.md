# nVideo Hybrid Architecture Refactor Plan

## Goal
Transform `nVideo` from a pure C++ Native implementation into a "Best-of-Both-Worlds" hybrid module. 
- Use the **FFmpeg CLI binary** for complex, hardware-accelerated stream mapping (file-to-file transcoding). This completely eliminates the `0xC0000005` access violation crashes caused by manually managing `hw_frames_ctx` domains in C++.
- Retain the **C++ NAPI Module** for operations where low latency and memory-to-memory operations shine (probing metadata, memory-buffer frame extraction, pure audio chunking).

## Phase 1: Reverting Broken Native Code
1. Open `modules/nVideo`.
2. Review the uncommitted edits in `src/processor.cpp`. These changes were attempts to fix the HW memory access violation (removing `hw_frames_ctx` allocation and forcing manual transfers).
3. **Action:** Strip out the experimental hardware-acceleration mapping logic (`hw_frames_ctx`, manual `av_hwframe_transfer_data()` for GPU -> CPU -> GPU pipelines) inside the C++ `transcode` loop. 
4. Ensure the C++ loop compiles cleanly and supports basic software encoding, restoring it to its last reliable state.
5. (Optional) Rename native method `transcode` to `transcodeNative` in the C++ layer so the JavaScript layer can differentiate the two pipelines.

## Phase 2: Integrating the CLI Wrapper
1. Inside `modules/nVideo/scripts/` (or via `package.json`), ensure we have access to the `ffmpeg.exe` binary (the build script `download-ffmpeg.js` already seems to download the required DLLs, but we need to ensure the executable is available alongside it or globally accessible).
2. Inside `modules/nVideo/lib/index.js`, implement `spawnFfmpeg()` using `child_process.spawn`.
3. Recreate the precise `onProgress` callback behavior:
   - Pipe `-progress pipe:1`
   - Parse `out_time_us`, `frame`, `bitrate`, `speed`, etc.
   - Calculate percentage based on the source duration (extracted via `nVideo.probe`).
4. Re-map the `nVideo.transcode(input, output, options)` JavaScript function to route intelligently:
   - By default, `transcode()` fires the robust CLI wrapper.
   - If `options.useNative === true`, it triggers the C++ NAPI loop.

## Phase 3: Preserving Native Superpowers
1. Verify `nVideo.probe()` remains 100% untouched and native.
2. Verify `nVideo.thumbnail()` and `nVideo.extractAudio()` remain 100% native (these operate without complex filter graphs or HW context sharing, so they don't trigger the access violations).
3. Ensure the caching mechanism currently inside `lib/index.js` wraps cleanly around the new CLI worker output.

## Phase 4: Testing & Verification
1. Run a transcoding job with `hwaccel: 'cuda'` using the new CLI pipeline. Verify zero-copy speed and zero crashes.
2. Run a pure audio extraction job to verify native pipelines are intact.
3. Validate that `onProgress`, `onComplete`, and `onError` callbacks behave identically between the NAPI and CLI paths, ensuring backward compatibility with any consuming code (like `MediaService` or Electron apps).
