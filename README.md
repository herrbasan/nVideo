# nVideo

Native Node.js module (N-API) for video and audio processing. Direct FFmpeg library integration with zero-copy performance. Built for Electron.

## What It Does

- **Probe** media files — metadata, streams, codec info, no process spawn
- **Extract thumbnails** — seek to timestamp, decode single frame
- **Generate waveforms** — audio peak data for visualization
- **Transcode to file** — FFmpeg processes entirely in C++ memory, zero JS overhead, real-time progress
- **Stream to JavaScript** — zero-copy decoded frames/audio into SharedArrayBuffer for real-time playback in Electron
- **AudioWorklet playback** — SAB ring buffer + AudioWorklet for gapless audio streaming
- **VideoFrame rendering** — canvas-based video streaming with frame queue

## Why Not fluent-ffmpeg / ffmpeg CLI?

| | nVideo | fluent-ffmpeg | ffmpeg CLI |
|---|--------|---------------|------------|
| Process spawn | No | Yes (every call) | Yes |
| Zero-copy decode | Yes | No | No |
| Real-time streaming | Yes (SAB) | No | No |
| Progress reporting | Rich, real-time | Basic | Pipe parsing |
| Memory overhead | Near zero | Moderate | High |
| JS heap involvement | None for file ops | High | N/A |

## Quick Start

```javascript
const nVideo = require('nvideo');

// Probe — ffprobe equivalent
const info = nVideo.probe('input.mp4');

// Thumbnail
const thumb = nVideo.thumbnail('input.mp4', { timestamp: 5.0, width: 320 });

// Transcode — FFmpeg runs entirely in C++, Node.js is idle
nVideo.transcode('input.mkv', 'output.mp4', {
  video: { codec: 'libx264', width: 1280, height: 720, crf: 23 },
  audio: { codec: 'aac', bitrate: 128000 },
  onProgress: (p) => console.log(`${p.percent.toFixed(1)}% at ${p.speed.toFixed(1)}x`),
  onComplete: (r) => console.log(`Done in ${(r.timeMs / 1000).toFixed(1)}s`)
});

// Remux — stream copy without re-encode
nVideo.remux('input.mkv', 'output.mp4');

// Extract audio from video
nVideo.extractAudio('video.mp4', 'output.wav');

// Stream audio decode (zero-copy)
const input = nVideo.openInput('input.mp4');
const audioBuffer = new Float32Array(4096);
let samplesRead;
while ((samplesRead = input.readAudio(audioBuffer)) > 0) {
  // audioBuffer contains interleaved float32 stereo — zero-copy
}
input.close();
```

### Streaming Playback (AudioWorklet)

```javascript
const nVideo = require('nvideo');

const audioContext = new AudioContext();
const player = new nVideo.AudioStreamPlayer(audioContext);

await player.open('song.mp3');
player.onEnded(() => console.log('Track finished'));
player.onPosition((time, duration) => console.log(`${time.toFixed(1)}s / ${duration.toFixed(1)}s`));

player.play();
player.seek(30);  // jump to 30 seconds
player.pause();
player.setLoop(true);
player.dispose();
```

### Streaming Playback (Video)

```javascript
const nVideo = require('nvideo');

const player = new nVideo.VideoStreamPlayer({
  canvas: document.getElementById('canvas'),
  width: 1280,
  height: 720
});

await player.open('video.mp4');
player.onFrame((frame) => console.log(`Frame ${frame.frameNum} at ${frame.pts.toFixed(2)}s`));
player.play();
player.getCurrentTime();  // current playback position
player.dispose();
```

### Synchronized A/V Playback

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

## API

### Utility Functions

| Function | Description |
|----------|-------------|
| `nVideo.probe(path)` | Metadata, streams, codec info |
| `nVideo.getMetadata(path)` | Lightweight file metadata only |
| `nVideo.thumbnail(path, opts)` | Extract single frame as RGB pixels |
| `nVideo.transcode(input, output, opts)` | Full transcode pipeline to file |
| `nVideo.remux(input, output, opts)` | Stream copy without re-encode |
| `nVideo.convert(input, output, opts)` | Format conversion shorthand |
| `nVideo.concat(files, output, opts)` | Join multiple files |
| `nVideo.extractStream(input, output, index, opts)` | Copy single stream to file |
| `nVideo.extractAudio(input, output, opts)` | Decode audio, re-encode to target format |
| `nVideo.clearCache(opts)` | Clear transcode cache |

### Core API (FFmpeg-native)

| Function | FFmpeg C API |
|----------|-------------|
| `nVideo.openInput(path)` | `avformat_open_input` |
| `input.readAudio(numSamples)` | `avcodec_receive_frame` + `swr_convert` |
| `input.readVideoFrame(buffer)` | `avcodec_receive_frame` + `sws_scale` |
| `input.seek(seconds)` | `av_seek_frame` |
| `input.close()` | `avformat_close_input` |

### Streaming Players

| Class | Description |
|-------|-------------|
| `nVideo.AudioStreamPlayer` | SAB ring buffer + AudioWorklet playback |
| `nVideo.VideoStreamPlayer` | Frame queue + canvas rendering |
| `nVideo.AVStreamPlayer` | Synchronized audio + video playback |
| `nVideo.createBufferPool(opts)` | Pre-allocated buffer pool for zero-GC |
| `nVideo.createRingBuffer(capacity, channels)` | Pure JS ring buffer |

### AudioStreamPlayer Methods

| Method | Description |
|--------|-------------|
| `await player.open(filePath)` | Open and decode audio file |
| `player.play(when)` | Start playback (optional scheduled start time) |
| `player.pause()` | Pause playback, disconnect worklet (CPU save) |
| `player.resume()` | Resume playback |
| `player.stop(keepSAB)` | Stop and release decoder |
| `player.seek(seconds)` | Jump to timestamp |
| `player.getCurrentTime()` | Current playback position (seconds) |
| `player.setLoop(enabled)` | Enable/disable gapless looping |
| `player.setVolume(vol)` | Set volume (0.0 - 1.0) |
| `player.onEnded(callback)` | Track finished callback |
| `player.onPosition(callback)` | Position update callback (time, duration) |
| `player.getBufferedFrames()` | Frames buffered in ring |
| `player.getUnderrunCount()` | Number of buffer underruns |
| `player.dispose()` | Release all resources |

### VideoStreamPlayer Methods

| Method | Description |
|--------|-------------|
| `await player.open(filePath)` | Open and decode video file |
| `player.play()` | Start playback |
| `player.pause()` | Pause playback |
| `player.resume()` | Resume playback |
| `player.stop(keepSAB)` | Stop and release decoder |
| `player.seek(seconds)` | Jump to timestamp |
| `player.getCurrentTime()` | Current playback position (seconds) |
| `player.setLoop(enabled)` | Enable/disable looping |
| `player.onEnded(callback)` | Playback finished callback |
| `player.onFrame(callback)` | Per-frame callback (frameNum, pts, keyframe, buffered) |
| `player.getBufferedFrames()` | Frames buffered in ring |
| `player.getUnderrunCount()` | Number of buffer underruns |
| `player.dispose()` | Release all resources |

All async operations support `onProgress`, `onComplete`, `onError` callbacks with detailed real-time stats (timestamp, percent, speed, bitrate, ETA, frame counts).

## Build

### Prerequisites

- Node.js >= 18
- Visual Studio 2022 (MSVC v143)
- Python 3.x (for node-gyp)

### Setup

```powershell
npm install
npm run setup        # download FFmpeg shared libs
npm run build        # compile native module
npm test             # run tests
```

### Electron

```powershell
npx electron-rebuild -f -w nvideo -v <electron-version>
```

## Platform Support

| Platform | Status |
|----------|--------|
| Windows x64 (MSVC) | ✅ Supported |
| Linux x64 | Planned |
| macOS x64 / ARM64 | Planned |

## Performance

| Operation | Measured | Target |
|-----------|----------|--------|
| `probe()` | 1.28ms | < 10ms |
| `thumbnail()` | 4.55ms | < 50ms |
| Audio decode | 0.18ms/chunk | < 5ms |
| Video decode (4K H.264) | 31.6 fps | — |
| Transcode (OGG→MP3 320k) | 1.06x ffmpeg CLI | Parity |
| NVENC transcode | 12.5x software | — |

## Documentation

- [nVideo_spec.md](docs/nVideo_spec.md) — Architecture specification
- [nVideo_dev_plan.md](docs/nVideo_dev_plan.md) — Development plan and phases
- [AGENTS.md](AGENTS.md) — LLM development guide

## License

[MIT](LICENSE)
