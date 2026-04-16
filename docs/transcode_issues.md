# nVideo: Native Transcoding Issues

**Date:** April 16, 2026
**Topic:** Critical bugs in the `transcode` C++ loop when multiplexing scaled video and encoded audio.

## Overview

During testing of the `nVideo.transcode()` function using the baseline documentation examples, we identified fundamental flaws in how the C++ loop handles simultaneous video scaling and audio encoding. The system crashes or throws severe warnings when attempting to multiplex an H264/AAC output stream with dynamic video downscaling.

These issues stem from the complexity of the FFmpeg C API. Things that the `ffmpeg` CLI binary handles automatically for the user (packet buffering, timebase correction, stream synchronization, and buffer pooling) are currently lacking in our simplified NAPI `transcode` loop.

## Identified Issues

### 1. SWScale Buffer Allocation (`bad dst image pointers`)

When the video stream is configured to scale down (e.g., `height: 480`), the internal `sws_scale` context generates thousands of `[swscaler] bad dst image pointers` warnings.

**Root Cause:**
In `modules/nVideo/src/processor.cpp`, the destination frame (`scaledFrame`) is allocated and its buffer is assigned via `av_frame_get_buffer(scaledFrame, 0)` before the transcode loop starts. However, either the line sizes are not aligning with what `sws_scale` expects for the specific target `pix_fmt` (like `yuv420p`), or reusing a single frame buffer repeatedly without proper reference resetting (or unreferencing) causes the pointers to become invalid or treated as non-writable during execution.

### 2. AAC Encoder Timestamp Strictness (`Could not update timestamps for skipped samples`)

When multiplexing audio, the AAC encoder throws continuous timestamp warnings (`Timestamps are unset in a packet`) and eventually fails with `[aac] Could not update timestamps for skipped samples`, often crashing the Node process with `ECONNRESET`.

**Root Cause:**
The FFmpeg AAC encoder is notoriously strict about requiring perfectly sequential, continuous Presentation Time Stamps (PTS). Our naive `while(av_read_frame)` loop reads packets, attempts to send them to the audio encoder, and writes them out. However, if samples are dropped, resampling occurs without correct PTS compensation, or the timebase of the input audio doesn't seamlessly translate to the timebase of the output audio stream, gaps appear in the timeline. The NAPI loop does not currently implement a robust Monotonic PTS counter or a sample-buffering pipeline to artificially ensure perfect chronological continuity for the AAC encoder.

### 3. Complex Interleaved Synchronization

The `transcode()` function attempts to process video and audio packets strictly in the order they are demuxed. However, because video frames (especially B-frames) and audio buffers encode at different latencies, naively wrapping `av_interleaved_write_frame` immediately after every `avcodec_receive_packet` without a dedicated interleave buffer or proper packet DTS/PTS chronological sorting leads to invalid muxing states.

## Conclusion & Next Steps

The `nVideo.transcode` function is fundamentally broken for scaling and multiplexing interleaved A/V streams. To fix this, the NAPI C++ backend in the `nVideo` project needs a rewrite of the `transcode` loop to implement:

1. Correct `SwsContext` buffer pooling, ensuring `dst` pointers have correct stride alignments and are writable on every frame.
2. A robust FIFO (First-In-First-Out) queue for audio samples (like `AVAudioFifo`) to ensure the AAC encoder receives exactly `frame_size` samples per frame with perfectly calculated, continuous PTS.
3. Proper timebase conversion mathematics (`av_packet_rescale_ts`) across the entire demux -> decode -> encode -> mux timeline.

**Temporary Workaround for Media Service:**
Until `nVideo`'s native multiplexing is repaired upstream, the Media Service should bypass the native `transcode` function when processing both audio and video simultaneously. Instead, operations should be performed sequentially:
1. Extract audio to a temporary file via `extractAudio`.
2. Extract/scale video to a temporary file via single-stream video processing.
3. Merge the two discrete files together using a simple CLI spawn or a specialized native remux-only function.