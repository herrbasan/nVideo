# FFmpeg Capabilities Reference

Guide to querying and using FFmpeg capabilities in nVideo.

## Table of Contents

- [Quick Reference](#quick-reference)
- [Live Query vs Static Files](#live-query-vs-static-files)
- [Video Codecs](#video-codecs)
- [Audio Codecs](#audio-codecs)
- [Hardware Acceleration](#hardware-acceleration)
- [Container Formats](#container-formats)
- [Filters](#filters)
- [Protocols](#protocols)

---

## Quick Reference

```javascript
const nVideo = require('nvideo');

// Get all capabilities (live from binary)
const buildInfo = nVideo.getBuildInfo();
const codecs = nVideo.getCodecs('video');
const filters = nVideo.getFilters('video');
const formats = nVideo.getFormats();

// Get pre-generated capabilities (fast, from JSON)
const caps = nVideo.getCapabilities();
console.log(caps.commonCodecs.encoders.video.nvidia);

// Direct JSON import (fastest)
const commonCodecs = require('nvideo/lib/capabilities/codecs-common.json');
```

---

## Live Query vs Static Files

### Live Query

Query FFmpeg binary directly (current state):

```javascript
// Current FFmpeg build info
const buildInfo = nVideo.getBuildInfo();

// Available codecs
const videoCodecs = nVideo.getCodecs('video');  // 444 codecs
const audioCodecs = nVideo.getCodecs('audio');  // 306 codecs

// Available filters
const videoFilters = nVideo.getFilters('video');  // 242 filters
const audioFilters = nVideo.getFilters('audio');  // 129 filters

// Container formats
const formats = nVideo.getFormats();  // 416 formats
```

### Static Files

Pre-generated JSON files (fast, no native module call):

```javascript
// Via API
const caps = nVideo.getCapabilities();

// Direct import
const index = require('nvideo/lib/capabilities/index.json');
const codecs = require('nvideo/lib/capabilities/codecs.json');
const commonCodecs = require('nvideo/lib/capabilities/codecs-common.json');
const filters = require('nvideo/lib/capabilities/filters.json');
const formats = require('nvideo/lib/capabilities/formats.json');
const buildInfo = require('nvideo/lib/capabilities/build-info.json');
```

### When to Use What

| Method | Speed | Use Case |
|--------|-------|----------|
| Direct import | Fastest | Static reference, build-time decisions |
| `getCapabilities()` | Fast | Runtime reference without native call |
| Live query | Moderate | Dynamic checks, runtime validation |

---

## Video Codecs

### Encoder Selection

#### CPU Encoders (Best Quality)

| Codec | Speed | Quality | Use Case |
|-------|-------|---------|----------|
| `libx264` | Medium | Excellent | H.264/AVC, universal compatibility |
| `libx265` | Slow | Superior | H.265/HEVC, better compression |
| `libsvtav1` | Medium | Excellent | AV1, modern, streaming |
| `libaom-av1` | Slow | Best | AV1, maximum quality |

```javascript
const caps = nVideo.getCapabilities();

// CPU encoders
const cpuEncoders = caps.commonCodecs.encoders.video.cpu;
// [libx264, libx265, libsvtav1, libaom-av1]

// Quick reference
const recommended = caps.commonCodecs.recommended;
// { webStreaming: 'libx264', archiving: 'libx265', modern: 'libsvtav1' }
```

#### NVIDIA NVENC (Fast, Good Quality)

| Codec | Speed | Quality | GPU |
|-------|-------|---------|-----|
| `h264_nvenc` | Very Fast | Good | GTX 600+ |
| `hevc_nvenc` | Very Fast | Good | GTX 900+ |
| `av1_nvenc` | Fast | Very Good | RTX 40-series |

```javascript
// Check if NVENC available
const buildInfo = nVideo.getBuildInfo();
if (buildInfo.hwaccels.includes('nvenc')) {
  // Use NVENC
}

// Get NVENC encoders
const nvencEncoders = nVideo.getCodecs('video')
  .filter(c => c.name.includes('nvenc') && c.canEncode);
```

#### Intel QSV (Integrated GPU)

| Codec | Speed | Quality | CPU |
|-------|-------|---------|-----|
| `h264_qsv` | Fast | Good | 4th Gen+ |
| `hevc_qsv` | Fast | Good | 6th Gen+ |
| `av1_qsv` | Fast | Very Good | 14th Gen+ |

```javascript
// Check QSV support
if (nVideo.getBuildInfo().hwaccels.includes('qsv')) {
  const qsvEncoders = nVideo.getCodecs('video')
    .filter(c => c.name.includes('qsv') && c.canEncode);
}
```

#### AMD AMF

| Codec | Speed | Quality | GPU |
|-------|-------|---------|-----|
| `h264_amf` | Fast | Good | RX 400+ |
| `hevc_amf` | Fast | Good | RX 6000+ |
| `av1_amf` | Fast | Very Good | RX 7000+ |

### Decoder Selection

Common decoders (auto-selected by FFmpeg):

```javascript
const caps = nVideo.getCapabilities();
const decoders = caps.commonCodecs.decoders.video;
// [h264, hevc, av1, libdav1d, vp9, vp8, mpeg4, mpeg2video]
```

**Note:** `libdav1d` is the fastest AV1 decoder (VideoLAN).

---

## Audio Codecs

### Encoders

| Codec | Bitrate | Quality | Use Case |
|-------|---------|---------|----------|
| `aac` | 128-256k | Good | Universal compatibility |
| `libfdk_aac` | 128-256k | Excellent | Best AAC encoder |
| `libopus` | 96-160k | Excellent | Modern, low-latency, streaming |
| `flac` | Variable | Lossless | Archiving, editing |
| `libmp3lame` | 192-320k | Good | Legacy compatibility |
| `ac3` | 384-640k | Good | DVD/Blu-ray |
| `eac3` | 192-1024k | Good | Dolby Digital+ |
| `pcm_s16le` | ~1411k | Lossless | Uncompressed, editing |

```javascript
const caps = nVideo.getCapabilities();
const audioEncoders = caps.commonCodecs.encoders.audio;
// [aac, flac, libmp3lame, libopus, ac3, eac3, vorbis, pcm_s16le]
```

### Decoders

```javascript
const caps = nVideo.getCapabilities();
const audioDecoders = caps.commonCodecs.decoders.audio;
// [aac, mp3, flac, opus, vorbis, ac3, eac3, pcm_s16le]
```

---

## Hardware Acceleration

### Check Available Hardware

```javascript
const buildInfo = nVideo.getBuildInfo();

console.log('Hardware accelerations:', buildInfo.hwaccels);
// ['cuda', 'vaapi', 'dxva2', 'qsv', 'd3d11va', 'opencl', 'vulkan', 'd3d12va', 'amf']
```

### Use Hardware Acceleration

```javascript
// Enable CUDA decoding + NVENC encoding
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: {
    codec: 'h264_nvenc',
    bitrate: 5000000
  },
  hwaccel: 'cuda'
});

// Enable QSV
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'h264_qsv' },
  hwaccel: 'qsv'
});
```

### Decoder-only Hardware

Some hardware accelerations are decode-only:

| Hardware | Decode | Encode | Notes |
|----------|--------|--------|-------|
| CUDA | Yes | Yes | NVIDIA NVENC |
| QSV | Yes | Yes | Intel Quick Sync |
| AMF | Yes | Yes | AMD Advanced Media Framework |
| DXVA2 | Yes | No | DirectX Video Acceleration |
| D3D11VA | Yes | No | Direct3D 11 Video |
| D3D12VA | Yes | Yes | Direct3D 12 Video |
| VAAPI | Yes | Yes | Linux Video Acceleration |
| Vulkan | Yes | Yes | Cross-platform |

---

## Container Formats

### Supported Formats

```javascript
const formats = nVideo.getFormats();

// Find format by extension
const mp4 = formats.find(f => f.extensions.includes('mp4'));
console.log(mp4.name); // "mov,mp4,m4a,3gp,3g2,mj2"
console.log(mp4.canMux, mp4.canDemux); // true, true

// Filter by capability
const muxers = formats.filter(f => f.canMux);
const demuxers = formats.filter(f => f.canDemux);
```

### Common Formats

| Format | Extensions | Mux | Demux | Notes |
|--------|------------|-----|-------|-------|
| MP4 | mp4, m4a, mov | Yes | Yes | Universal |
| Matroska/WebM | mkv, webm | Yes | Yes | Open, flexible |
| AVI | avi | Yes | Yes | Legacy |
| MPEG-TS | ts, m2ts | Yes | Yes | Broadcast |
| FLV | flv | Yes | Yes | Streaming |
| MP3 | mp3 | Yes | Yes | Audio only |
| WAV | wav | Yes | Yes | Uncompressed audio |
| FLAC | flac | Yes | Yes | Lossless audio |
| AAC | aac | Yes | Yes | Raw AAC |
| OGG | ogg, ogv | Yes | Yes | Open source |

---

## Filters

### Query Filters

```javascript
const videoFilters = nVideo.getFilters('video');
const audioFilters = nVideo.getFilters('audio');

// Find specific filter
const scale = videoFilters.find(f => f.name === 'scale');
console.log(scale.description); // "Scale the input video size..."

// Filter by description
const resizers = videoFilters.filter(f =>
  f.description.toLowerCase().includes('scale') ||
  f.description.toLowerCase().includes('resize')
);
```

### Video Filters

| Filter | Description | Example |
|--------|-------------|---------|
| `scale` | Resize video | `scale=1920:1080` |
| `crop` | Crop region | `crop=1280:720:0:0` |
| `fps` | Change frame rate | `fps=30` |
| `format` | Pixel format | `format=pix_fmts=yuv420p` |
| `transpose` | Rotate/flip | `transpose=1` |
| `hflip` | Horizontal flip | `hflip` |
| `vflip` | Vertical flip | `vflip` |
| `fade` | Fade in/out | `fade=in:0:30` |
| `eq` | Adjust brightness/contrast | `eq=brightness=0.1:contrast=1.2` |
| `overlay` | Overlay video | `overlay=x=0:y=0` |
| `pad` | Add padding | `pad=1920:1080:0:0` |
| `trim` | Trim video | `trim=start=10:end=20` |

### Audio Filters

| Filter | Description | Example |
|--------|-------------|---------|
| `volume` | Change volume | `volume=0.5` |
| `aresample` | Resample audio | `aresample=48000` |
| `amix` | Mix audio streams | `amix=inputs=2` |
| `pan` | Channel mapping | `pan=stereo\|c0=c0\|c1=c1` |
| `acompressor` | Dynamic range compression | `acompressor` |
| `equalizer` | Graphic equalizer | `equalizer=f=1000:width_type=h:width=200:g=-10` |
| `dynaudnorm` | Dynamic audio normalizer | `dynaudnorm` |
| `silencedetect` | Detect silence | `silencedetect=noise=-50dB:d=0.5` |

### Filter Graphs

Combine multiple filters:

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: {
    codec: 'libx264',
    filters: 'scale=1920:1080,format=pix_fmts=yuv420p,fps=30'
  },
  audio: {
    codec: 'aac',
    filters: 'volume=0.8,aresample=48000'
  }
});
```

### Filter Syntax

- **Filter chains** (sequential): `filter1,filter2,filter3`
- **Filter graphs** (parallel): `[in]filter1[out1];[in]filter2[out2]`
- **Multiple inputs**: `filter=input1:input2`
- **Escaping**: Use `\|` for `|` in filter strings

---

## Protocols

### Supported Protocols

```javascript
const buildInfo = nVideo.getBuildInfo();

console.log('Protocols:', buildInfo.protocols);
// ['file', 'http', 'https', 'ftp', 'rtmp', 'rtp', 'srt', ...]
```

### Common Protocols

| Protocol | Description | Example |
|----------|-------------|---------|
| `file` | Local files | `file:///path/to/video.mp4` |
| `http` | HTTP | `http://server.com/video.mp4` |
| `https` | HTTPS | `https://server.com/video.mp4` |
| `ftp` | FTP | `ftp://server.com/video.mp4` |
| `rtmp` | Real-Time Messaging Protocol | `rtmp://server.com/live/stream` |
| `rtp` | Real-time Transport Protocol | `rtp://@:5004` |
| `srt` | Secure Reliable Transport | `srt://server.com:9000` |
| `tcp` | TCP | `tcp://server.com:5000` |
| `udp` | UDP | `udp://@:5004` |

### Network Input

```javascript
// HTTP input
const info = nVideo.probe('https://example.com/video.mp4');

// RTMP input
await nVideo.transcode('rtmp://server.com/live/stream', 'output.mp4', {
  video: { codec: 'libx264' }
});

// RTMP output (streaming)
await nVideo.transcode('input.mp4', 'rtmp://server.com/live/stream', {
  video: { codec: 'libx264', fps: 30 }
});
```

---

## Regenerate Static Files

If FFmpeg is updated, regenerate capability files:

```bash
# Manually
npm run generate-capabilities

# Or automatically during build
npm run build
```

This will update `lib/capabilities/` with current FFmpeg capabilities.

---

## Static File Structure

```
lib/capabilities/
├── index.json              # Summary and file references
├── build-info.json         # FFmpeg version, config, protocols, hwaccels
├── codecs.json             # All 786 codecs
├── codecs-common.json      # Curated encoder/decoder lists
├── filters.json            # All 568 filters
└── formats.json            # All 416 formats
```

### codecs-common.json Structure

```javascript
{
  encoders: {
    video: {
      cpu: [...],        // Software encoders
      nvidia: [...],     // NVENC encoders
      intel: [...],      // QSV encoders
      amd: [...],        // AMF encoders
      other_hw: [...],   // Vulkan, etc.
      professional: [...] // ProRes, DNxHD, FFV1
    },
    audio: [...]         // Common audio encoders
  },
  decoders: {
    video: [...],        // Video decoders
    audio: [...]         // Audio decoders
  },
  videoEncodersByHwaccel: {
    cpu: ['libx264', 'libx265', 'libsvtav1'],
    nvidia: ['h264_nvenc', 'hevc_nvenc', 'av1_nvenc'],
    // ...
  },
  recommended: {
    webStreaming: { video: 'libx264', audio: 'aac' },
    archiving: { video: 'libx265', audio: 'flac' },
    modern: { video: 'libsvtav1', audio: 'libopus' },
    fastest: { video: 'h264_nvenc', audio: 'aac' }
  }
}
```
