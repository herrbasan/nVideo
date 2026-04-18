# Transcoding Guide

Complete guide to transcoding with nVideo.

## Table of Contents

- [Basic Transcoding](#basic-transcoding)
- [Hardware Acceleration](#hardware-acceleration)
- [Codec Selection](#codec-selection)
- [Quality Settings](#quality-settings)
- [Filters](#filters)
- [Progress Tracking](#progress-tracking)
- [Caching](#caching)
- [Common Recipes](#common-recipes)

---

## Basic Transcoding

### Simple Transcode

```javascript
const nVideo = require('nvideo');

await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'libx264' },
  audio: { codec: 'aac' }
});
```

### With Options

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: {
    codec: 'libx264',
    width: 1920,
    height: 1080,
    crf: 23,
    preset: 'medium'
  },
  audio: {
    codec: 'aac',
    bitrate: 128000
  },
  threads: 4
});
```

### Video Options Reference

| Option | Type | Description | Default |
|--------|------|-------------|---------|
| `codec` | string | Video codec | Required |
| `width` | number | Output width | 0 (original) |
| `height` | number | Output height | 0 (original) |
| `crf` | number | Constant rate factor (quality) | -1 (codec default) |
| `preset` | string | Encoding speed/quality tradeoff | -1 |
| `pixelFormat` | string | Pixel format | -1 |
| `bitrate` | number | Target bitrate (bits/sec) | 0 (VBR) |
| `fps` | number | Frame rate | 0 (original) |
| `filters` | string | FFmpeg filter graph | - |

### Audio Options Reference

| Option | Type | Description | Default |
|--------|------|-------------|---------|
| `codec` | string | Audio codec | Required |
| `bitrate` | number | Target bitrate (bits/sec) | 0 (VBR) |
| `sampleRate` | number | Sample rate | 0 (original) |
| `channels` | number | Channel count | 0 (original) |
| `filters` | string | FFmpeg filter graph | - |

---

## Hardware Acceleration

### NVIDIA NVENC

Requires NVIDIA GPU with NVENC (GTX 600+ or RTX series).

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: {
    codec: 'h264_nvenc',
    bitrate: 5000000  // 5 Mbps
  },
  hwaccel: 'cuda'
});
```

Available NVENC codecs:
- `h264_nvenc` - H.264 (compatible everywhere)
- `hevc_nvenc` - H.265/HEVC (better compression)
- `av1_nvenc` - AV1 (RTX 40-series+, best compression)

### Intel QSV

Requires Intel CPU with integrated graphics (4th Gen+).

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'h264_qsv' },
  hwaccel: 'qsv'
});
```

### AMD AMF

Requires AMD GPU (RX 400+ series).

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'h264_amf' }
});
```

### Check Available Hardware

```javascript
const buildInfo = nVideo.getBuildInfo();
console.log('Hardware accelerations:', buildInfo.hwaccels);

// Get hardware encoders
const videoCodecs = nVideo.getCodecs('video');
const nvenc = videoCodecs.filter(c => c.name.includes('nvenc'));
console.log('NVENC encoders:', nvenc.map(c => c.name));
```

---

## Codec Selection

### Video Codecs

| Codec | Type | Speed | Quality | Use Case |
|-------|------|-------|---------|----------|
| `libx264` | CPU | Medium | Excellent | General purpose, compatibility |
| `libx265` | CPU | Slow | Superior | Archiving, storage efficiency |
| `libsvtav1` | CPU | Medium | Excellent | Modern, streaming, future-proof |
| `h264_nvenc` | GPU | Very Fast | Good | Real-time, recording |
| `hevc_nvenc` | GPU | Very Fast | Good | 4K recording, streaming |
| `av1_nvenc` | GPU | Fast | Very Good | Next-gen streaming (RTX 40+) |

### Audio Codecs

| Codec | Bitrate | Quality | Use Case |
|-------|---------|---------|----------|
| `aac` | 128-256k | Good | General purpose, compatibility |
| `libfdk_aac` | 128-256k | Excellent | Best AAC encoder (if available) |
| `libopus` | 96-160k | Excellent | Modern, low-latency, streaming |
| `flac` | Variable | Lossless | Archiving, editing |
| `libmp3lame` | 192-320k | Good | Legacy compatibility |
| `ac3` | 384-640k | Good | DVD/Blu-ray compatibility |

---

## Quality Settings

### CRF Values (x264/x265)

| CRF | Quality | Use Case |
|-----|---------|----------|
| 18 | Visually lossless | Archiving, editing |
| 23 | Default | General purpose |
| 28 | Good | Streaming, storage efficient |
| 35+ | Acceptable | Low bandwidth, preview |

### Presets (x264/x265)

| Preset | Speed | Quality | Use Case |
|--------|-------|---------|----------|
| `ultrafast` | Fastest | Lower | Real-time streaming |
| `superfast` | Very Fast | Good | Live encoding |
| `veryfast` | Fast | Good | Fast encoding |
| `faster` | Faster | Better | Quick encoding |
| `fast` | Fast | Better | Balanced speed |
| `medium` | Medium | Best balance | Default |
| `slow` | Slow | Better | Better quality |
| `slower` | Slower | Better | Archiving |
| `veryslow` | Slowest | Best | Final delivery |

---

## Filters

Apply FFmpeg filters during transcode:

### Scale

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: {
    codec: 'libx264',
    filters: 'scale=1280:720'  // width:height
  }
});
```

### Crop

```javascript
filters: 'crop=1280:720:0:0'  // width:height:x:y
```

### Multiple Filters

```javascript
filters: 'scale=1280:720,format=pix_fmts=yuv420p,fps=30'
```

### Common Video Filters

| Filter | Example | Description |
|--------|---------|-------------|
| `scale` | `scale=1920:1080` | Resize video |
| `crop` | `crop=1280:720:100:100` | Crop region |
| `fps` | `fps=30` | Force frame rate |
| `format` | `format=pix_fmts=yuv420p` | Pixel format |
| `transpose` | `transpose=1` | Rotate 90° clockwise |
| `hflip` | `hflip` | Horizontal flip |
| `vflip` | `vflip` | Vertical flip |
| `eq` | `eq=brightness=0.1:contrast=1.2` | Adjust brightness/contrast |

---

## Progress Tracking

### Basic Progress

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'libx264' },
  onProgress: (p) => {
    console.log(`${p.percent.toFixed(1)}% complete`);
    console.log(`Speed: ${p.speed.toFixed(2)}x`);
    console.log(`ETA: ${p.eta.toFixed(0)}s`);
  }
});
```

### Detailed Progress

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'libx264' },
  onProgress: (p) => {
    console.log({
      percent: p.percent,
      time: p.time,              // Current timestamp
      speed: p.speed,            // Processing speed
      bitrate: p.bitrate,        // Output bitrate
      size: p.size,              // Bytes written
      frames: p.frames,          // Video frames
      fps: p.fps,                // Encode speed
      audioTime: p.audioTime,    // Audio position
      estimatedSize: p.estimatedSize,
      eta: p.eta,                // Seconds remaining
      dupFrames: p.dupFrames,    // Duplicates
      dropFrames: p.dropFrames   // Dropped
    });
  }
});
```

---

## Caching

Enable caching to avoid re-transcoding the same file with same settings:

```javascript
await nVideo.transcode('input.mp4', 'output.mp4', {
  video: { codec: 'libx264', crf: 23 },
  cache: true,                      // Enable caching
  cacheDir: './.nvideo-cache',      // Custom cache directory
  cacheTTL: 10 * 60 * 1000,         // 10 minutes
  onCacheHit: (file) => console.log('Using cached version'),
  onCacheMiss: () => console.log('Cache miss, transcoding...')
});
```

### Cache Configuration

| Option | Type | Description | Default |
|--------|------|-------------|---------|
| `cache` | boolean | Enable caching | `true` |
| `cacheDir` | string | Cache directory | `.nvideo-cache` |
| `cacheTTL` | number | TTL in milliseconds | `300000` (5 min) |

### Clear Cache

```javascript
// Clear all cache
nVideo.clearCache();

// Clear entries older than 7 days
nVideo.clearCache({ olderThan: 7 * 24 * 60 * 60 * 1000 });

// Clear specific directory
nVideo.clearCache({ cacheDir: './my-cache' });
```

---

## Common Recipes

### YouTube Upload

```javascript
await nVideo.transcode('input.mp4', 'youtube.mp4', {
  video: {
    codec: 'libx264',
    width: 1920,
    height: 1080,
    crf: 18,
    preset: 'slow'
  },
  audio: {
    codec: 'aac',
    bitrate: 256000,
    sampleRate: 48000
  }
});
```

### Web Streaming (HLS/DASH)

```javascript
await nVideo.transcode('input.mp4', 'stream.mp4', {
  video: {
    codec: 'libx264',
    width: 1280,
    height: 720,
    crf: 28,
    preset: 'veryfast',
    fps: 30
  },
  audio: {
    codec: 'aac',
    bitrate: 128000
  }
});
```

### Archiving (HEVC)

```javascript
await nVideo.transcode('input.mp4', 'archive.mp4', {
  video: {
    codec: 'libx265',
    crf: 23,
    preset: 'slow'
  },
  audio: {
    codec: 'flac'  // Lossless audio
  }
});
```

### Mobile Device

```javascript
await nVideo.transcode('input.mp4', 'mobile.mp4', {
  video: {
    codec: 'libx264',
    width: 1280,
    height: 720,
    crf: 28,
    preset: 'fast'
  },
  audio: {
    codec: 'aac',
    bitrate: 128000,
    channels: 2
  }
});
```

### Screen Recording (NVENC)

```javascript
await nVideo.transcode('input.mp4', 'recording.mp4', {
  video: {
    codec: 'h264_nvenc',
    bitrate: 8000000,  // 8 Mbps
    fps: 60
  },
  audio: {
    codec: 'aac',
    bitrate: 192000
  },
  hwaccel: 'cuda'
});
```

### Extract Audio Only

```javascript
await nVideo.transcode('video.mp4', 'audio.mp3', {
  video: null,  // Disable video
  audio: {
    codec: 'libmp3lame',
    bitrate: 320000
  }
});
```

### Thumbnail Every N Seconds

```javascript
// Not directly supported by transcode,
// use nVideo.thumbnail() in a loop
for (let t = 0; t < duration; t += 10) {
  const thumb = nVideo.thumbnail('input.mp4', { timestamp: t, width: 320 });
  fs.writeFileSync(`thumb-${t}.raw`, thumb.data);
}
```

---

## Error Handling

```javascript
try {
  await nVideo.transcode('input.mp4', 'output.mp4', {
    video: { codec: 'libx264' },
    onError: (err) => {
      // Granular error from native layer
      console.error(`${err.operation} failed:`, err.message);
      console.error(`At frame ${err.frame}, stream ${err.stream}`);
    }
  });
} catch (err) {
  // High-level error
  console.error('Transcode failed:', err.message);
}
```

---

## Performance Tips

1. **Match input/output** - Don't change resolution/FPS unless needed
2. **Use hardware encoding** - NVENC/QSV for real-time encoding
3. **Adjust threads** - Match physical CPU cores
4. **Preset selection** - `veryfast` for live, `slow` for archival
5. **CRF vs bitrate** - CRF for storage, bitrate for streaming
6. **Audio bitrate** - 128k AAC is transparent for most content
7. **Pixel format** - yuv420p for compatibility, yuv444p for quality
