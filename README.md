# nVideo

> **Early development — this module does nothing yet. No code has been implemented.**

Native Node.js module (N-API) for video and audio processing. Direct FFmpeg library integration with zero-copy performance. Built for Electron.

## What It Does

- **Probe** media files — metadata, streams, codec info, no process spawn
- **Extract thumbnails** — seek to timestamp, decode single frame
- **Generate waveforms** — audio peak data for visualization
- **Transcode to file** — FFmpeg processes entirely in C++ memory, zero JS overhead, real-time progress
- **Stream to JavaScript** — zero-copy decoded frames/audio into SharedArrayBuffer for real-time playback in Electron

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

// Waveform
const waveform = nVideo.waveform('input.mp4', { points: 1000 });

// Transcode — FFmpeg runs entirely in C++, Node.js is idle
nVideo.transcode('input.mkv', 'output.mp4', {
  video: { codec: 'libx264', width: 1280, height: 720, crf: 23 },
  audio: { codec: 'aac', bitrate: 128000 },
  onProgress: (p) => console.log(`${p.percent.toFixed(1)}% at ${p.speed.toFixed(1)}x`),
  onComplete: (r) => console.log(`Done in ${(r.timeMs / 1000).toFixed(1)}s`)
});

// Stream audio to AudioWorklet (zero-copy via SharedArrayBuffer)
const input = nVideo.openInput('input.mp4');
input.openDecoder(1);
const audioBuffer = new Float32Array(4096);
let samplesRead;
while ((samplesRead = input.readAudio(audioBuffer)) > 0) {
  // audioBuffer contains interleaved float32 stereo — zero-copy
}
input.close();
```

## API

### Utility Functions

| Function | Description |
|----------|-------------|
| `nVideo.probe(path)` | Metadata, streams, codec info |
| `nVideo.thumbnail(path, opts)` | Extract single frame as RGB pixels |
| `nVideo.waveform(path, opts)` | Audio peak amplitudes |
| `nVideo.transcode(input, output, opts)` | Full transcode pipeline to file |
| `nVideo.remux(input, output, opts)` | Stream copy without re-encode |
| `nVideo.convert(input, output, opts)` | Format conversion shorthand |
| `nVideo.concat(files, output, opts)` | Join multiple files |

### Core API (FFmpeg-native)

| Function | FFmpeg C API |
|----------|-------------|
| `nVideo.openInput(path)` | `avformat_open_input` |
| `input.openDecoder(streamIndex)` | `avcodec_find_decoder` + `avcodec_open2` |
| `input.readVideoFrame(buffer)` | `avcodec_receive_frame` + `sws_scale` |
| `input.readAudio(buffer)` | `avcodec_receive_frame` + `swr_convert` |
| `input.seek(seconds)` | `av_seek_frame` |
| `input.setVideoFilter(graph)` | `avfilter_graph_parse_ptr` |
| `nVideo.openOutput(path)` | `avformat_alloc_output_context2` |

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
| Windows x64 (MSVC) | Target |
| Linux x64 | Planned |
| macOS x64 / ARM64 | Planned |

## Documentation

- [nVideo_spec.md](nVideo_spec.md) — Architecture specification
- [nVideo_dev_plan.md](nVideo_dev_plan.md) — Development plan and phases
- [AGENTS.md](AGENTS.md) — LLM development guide

## License

[MIT](LICENSE)
