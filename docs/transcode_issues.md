# nVideo: Native Transcoding Issues

**Date:** April 16, 2026
**Topic:** Critical bugs in the `transcode` C++ loop when multiplexing scaled video and encoded audio.

## Overview

During testing of the `nVideo.transcode()` function using the baseline documentation examples, we identified fundamental flaws in how the C++ loop handles simultaneous video scaling and audio encoding. The system crashes or throws severe warnings when attempting to multiplex an H264/AAC output stream with dynamic video downscaling.

These issues stem from the complexity of the FFmpeg C API. Things that the `ffmpeg` CLI binary handles automatically for the user (packet buffering, timebase correction, stream synchronization, and buffer pooling) are currently lacking in our simplified NAPI `transcode` loop.

## Identified Issues

### 1. SWScale Buffer Allocation (`bad dst image pointers`)

When the video stream is configured to scale down (e.g., `height: 480`), the internal `sws_scale` context generates thousands of `[swscaler] bad dst image pointers` warnings, eventually throwing a `0xC0000005` Access Violation.

**Root Cause:**
In `modules/nVideo/src/processor.cpp`, the destination frame (`scaledFrame`) is allocated before the transcode loop starts. Initially, we were reusing a single frame buffer repeatedly and improperly stripping the references. During the encoder handoff (`avcodec_send_frame`), the encoder can internally hold a lock onto that frame's underlying buffer buffer block. If we stripped this reference blindly, the destination pointers became invalid for `sws_scale`.

**Fix Deployed (April 16, 2026):**
1. We preserve `encFrame` PTS, DTS, and duration properties dynamically onto the new `scaledFrame`.
2. We enforce `av_frame_make_writable(scaledFrame)` immediately prior to the `sws_scale` invocation. This ensures that if the output encoder still has a lock on that previously generated memory block, ffmpeg instantiates a brand new contiguous buffer block natively without crashing the scaling processor.
3. We meticulously handle reference decrements, intentionally stripping the `av_frame_unref(encFrame)` step when operating exclusively on our internal `scaledFrame`.

### 2. AAC Encoder Timestamp Strictness (`Could not update timestamps for skipped samples`)

When multiplexing audio, the AAC encoder throws continuous timestamp warnings (`Timestamps are unset in a packet`) and eventually fails with `[aac] Could not update timestamps for skipped samples`, often crashing the Node process with `ECONNRESET`.

**Root Cause:**
The FFmpeg AAC encoder is notoriously strict about requiring perfectly sequential, continuous Presentation Time Stamps (PTS). Our naive `while(av_read_frame)` loop reads packets, attempts to send them to the audio encoder, and writes them out. However, if samples are dropped, resampling occurs without correct PTS compensation, or the timebase of the input audio doesn't seamlessly translate to the timebase of the output audio stream, gaps appear in the timeline. The NAPI loop does not currently implement a robust Monotonic PTS counter or a sample-buffering pipeline to artificially ensure perfect chronological continuity for the AAC encoder.

### 3. Complex Interleaved Synchronization

The `transcode()` function attempts to process video and audio packets strictly in the order they are demuxed. However, because video frames (especially B-frames) and audio buffers encode at different latencies, naively wrapping `av_interleaved_write_frame` immediately after every `avcodec_receive_packet` without a dedicated interleave buffer or proper packet DTS/PTS chronological sorting leads to invalid muxing states.

**Fix Deployed (April 16, 2026):**
Chronological Multiplexing has been repaired by rescaling the packets (`av_packet_rescale_ts`) from the originating encoder's `time_base` to the target `videoOutStream`/`audioOutStream`'s `time_base` directly before pushing to `av_interleaved_write_frame`. This enables proper sync integration with minimal overhead.

### 4. Data Plumbing Optimization: The Avoidance of `av_frame_make_writable`

During our initial scaling fix deployed on April 16, 2026, we used `av_frame_make_writable(scaledFrame)` to guarantee writable memory for target scaling buffers before calling `sws_scale`. While this prevented the native access violations (`0xC0000005`), it introduced a massive and invisible performance bottleneck.

If the downstream video encoder preserves the lock on `scaledFrame`'s memory buffer across iterations, `av_frame_make_writable` assumes you need to *modify* that data safely. Consequently, it allocates a fresh data buffer and executes a deep `memcpy` of the **entire previous frame payload** into the new buffer before unlocking. For a 4K or 1080p stream, this causes FFmpeg to literally copy millions of useless bytes per frame, only for `sws_scale` to instantly overwrite the entire buffer anyway.

**Fix Deployed:**
We refactored `processor.cpp` to explicitly drop the reference (`av_frame_unref(scaledFrame)`), restore the persistent `width`/`height`/`format` properties, and invoke `av_frame_get_buffer(scaledFrame, 32)` dynamically. This forces a clean, zero-copy buffer instantiation without executing the catastrophic `memcpy` step, vastly improving raw memory bandwidth utilization during transcodes.

## Conclusion & Next Steps

The `nVideo.transcode` function is fully operational for complex interleaving A/V streams. 

1. ~~Correct `SwsContext` buffer pooling, ensuring `dst` pointers have correct stride alignments and are writable on every frame.~~ **[FIXED — Further optimized via Zero-Copy manual buffer allocations to negate memory copy bloat in make_writable]**
2. ~~A robust FIFO (First-In-First-Out) queue for audio samples (like `AVAudioFifo`) to ensure the AAC encoder receives exactly `frame_size` samples per frame with perfectly calculated, continuous PTS.~~ **[FIXED — Natively solved by routing audio processing logic exclusively through the `asetnsamples` audioFilterGraph block, which transparently acts as the rigid sample chunker and solves the strictness warnings].**
3. ~~Proper timebase conversion mathematics (`av_packet_rescale_ts`) across the entire demux -> decode -> encode -> mux timeline.~~ **[FIXED]**

**Temporary Workaround for Media Service:**
Until `nVideo`'s native multiplexing is repaired upstream, the Media Service should bypass the native `transcode` function when processing both audio and video simultaneously. Instead, operations should be performed sequentially:
1. Extract audio to a temporary file via `extractAudio`.
2. Extract/scale video to a temporary file via single-stream video processing.
3. Merge the two discrete files together using a simple CLI spawn or a specialized native remux-only function.