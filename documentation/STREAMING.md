# Streaming Guide

Real-time audio/video streaming and playback with nVideo.

## Table of Contents

- [Overview](#overview)
- [Streaming Decode](#streaming-decode)
- [Audio Playback](#audio-playback)
- [Video Playback](#video-playback)
- [Synchronized Playback](#synchronized-playback)
- [SharedArrayBuffer](#sharedarraybuffer)
- [Buffer Management](#buffer-management)

---

## Overview

nVideo provides three levels of streaming APIs:

1. **Low-level**: Direct frame-by-frame decode via `AudioInput`
2. **Mid-level**: Players with buffering (`AudioStreamPlayer`, `VideoStreamPlayer`)
3. **High-level**: Synchronized A/V playback (`AVStreamPlayer`)

All streaming uses zero-copy buffers for maximum performance.

---

## Streaming Decode

### Audio Streaming

```javascript
const nVideo = require('nvideo');

// Open file for streaming decode
const input = nVideo.openInput('audio.mp3', {
  sampleRate: 44100,  // Target sample rate
  threads: 0          // Auto thread count
});

console.log(`Duration: ${input.getDuration()}s`);
console.log(`Sample Rate: ${input.getSampleRate()}Hz`);
console.log(`Channels: ${input.getChannels()}`);

// Stream decode loop
const bufferSize = 4096; // samples per read

while (input.isOpen()) {
  // Returns { samples: number, data: Float32Array }
  const { samples, data } = input.readAudio(bufferSize);

  if (samples === 0) {
    console.log('End of file');
    break;
  }

  // data contains samples * channels Float32 values
  // Interleaved: [L0, R0, L1, R1, L2, R2, ...]

  // Process audio data...
  for (let i = 0; i < samples; i++) {
    const left = data[i * 2];
    const right = data[i * 2 + 1];
    // Do something with samples
  }
}

input.close();
```

### Video Streaming

```javascript
const input = nVideo.openInput('video.mp4');

// Open video stream (auto-detects first video stream)
input.openVideoStream();

const width = input.getVideoWidth();
const height = input.getVideoHeight();
const fps = input.getVideoFPS();

console.log(`${width}x${height} @ ${fps}fps`);

// Pre-allocate buffer for zero-copy
const buffer = new Uint8Array(width * height * 3); // RGB24

while (input.isOpen()) {
  // Decode frame into provided buffer
  const frame = input.readVideoFrame(buffer, {
    format: 'rgb24',  // Output format
    width: 0,         // 0 = keep original
    height: 0         // 0 = keep original
  });

  if (!frame.width) {
    console.log('End of video');
    break;
  }

  console.log(`Frame ${frame.frameNum} @ ${frame.pts}s`);
  console.log(`Keyframe: ${frame.keyframe}`);

  // buffer now contains RGB24 pixel data
  // Process frame...
}

input.close();
```

### Seeking

```javascript
// Seek to 30 seconds
input.seek(30.0);

// Get current position
const position = input.getPosition();
console.log(`At ${position}s`);
```

### Available Pixel Formats

| Format | Bytes/Pixel | Description |
|--------|-------------|-------------|
| `rgb24` | 3 | RGB, 8-bit per channel |
| `bgr24` | 3 | BGR, 8-bit per channel |
| `rgba` | 4 | RGB + Alpha |
| `bgra` | 4 | BGR + Alpha |
| `argb` | 4 | Alpha + RGB |
| `gray8` | 1 | Grayscale |

---

## Audio Playback

### AudioStreamPlayer

High-performance player using AudioWorklet + SharedArrayBuffer.

```javascript
const nVideo = require('nvideo');

// Create AudioContext (required for AudioWorklet)
const audioContext = new AudioContext();

// Create player with 2-second buffer
const player = new nVideo.AudioStreamPlayer(audioContext, {
  bufferDuration: 2.0,
  channels: 2
});

async function play() {
  // Open audio input
  const input = nVideo.openInput('music.mp3');
  await player.open(input);

  // Start playback
  player.play();

  // Control playback
  setTimeout(() => player.pause(), 5000);
  setTimeout(() => player.play(), 7000);
  setTimeout(() => player.seek(30.0), 10000);

  // Enable looping
  player.setLoop(true, 10.0, 20.0); // Loop 10-20 seconds
}

play();
```

### Player States

```javascript
player.getState(); // 'stopped', 'playing', 'paused'

// Events via polling
setInterval(() => {
  const time = player.getCurrentTime();
  console.log(`Playing: ${time.toFixed(2)}s`);
}, 1000);
```

### Cleanup

```javascript
// Disconnect from audio graph
player.disconnect();

// Close input
input.close();
```

### Buffer Underrun Handling

The player automatically handles buffer underruns by:
1. Monitoring ring buffer levels
2. Adjusting feed timing
3. Reporting underrun count

```javascript
// Access control buffer for debugging
// Control buffer layout (Int32Array):
// 0: writePtr
// 1: readPtr
// 2: state
// 3: sampleRate
// 4: channels
// 5: loop
// 6: loopStart
// 7: loopEnd
// 8: totalFrames
// 9: underrunCount
// 10-11: startTime (int64)
```

---

## Video Playback

### VideoStreamPlayer

Canvas-based player with frame queue.

```javascript
const canvas = document.getElementById('videoCanvas');

const player = new nVideo.VideoStreamPlayer({
  canvas: canvas,
  maxQueuedFrames: 8  // Frame queue depth
});

async function play() {
  // Open video
  await player.open('video.mp4');

  // Get info
  const info = player.getVideoInfo();
  console.log(`${info.width}x${info.height} @ ${info.fps}fps`);

  // Play
  player.play();

  // Control
  setTimeout(() => player.pause(), 5000);
  setTimeout(() => player.seek(30.0), 10000);
}

play();
```

### Canvas Sizing

```javascript
// Player automatically sizes canvas to video dimensions
// To maintain aspect ratio:
const info = player.getVideoInfo();
const aspect = info.width / info.height;

// Resize canvas container
const container = document.getElementById('videoContainer');
container.style.aspectRatio = aspect;
```

### Frame Drop Detection

The player automatically drops frames if rendering falls behind:

```javascript
// Player maintains frame queue
// If queue grows too large, oldest frames are dropped
// This ensures audio sync is maintained
```

---

## Synchronized Playback

### AVStreamPlayer

Combined audio + video playback with synchronization.

```javascript
const audioContext = new AudioContext();
const canvas = document.getElementById('canvas');

const av = new nVideo.AVStreamPlayer({
  audioContext: audioContext,
  canvas: canvas
});

async function play() {
  // Open same file for both audio and video
  await av.openAudio('movie.mp4');
  await av.openVideo('movie.mp4');

  // Play synchronized
  av.play();

  // Control (affects both)
  av.pause();
  av.seek(60.0);
  av.setLoop(true, 0, 120.0);

  // Get position (from audio, the master clock)
  console.log(av.getCurrentTime());
}

play();
```

### Synchronization Strategy

- **Audio** is the master clock (never drops)
- **Video** syncs to audio position
- If video falls behind, frames are dropped
- If video runs ahead, playback is delayed

---

## SharedArrayBuffer

### Requirements

For AudioWorklet support, you need cross-origin isolation:

```javascript
// Electron main process
const { session } = require('electron');

session.defaultSession.webRequest.onHeadersReceived((details, callback) => {
  callback({
    responseHeaders: {
      ...details.responseHeaders,
      'Cross-Origin-Opener-Policy': ['same-origin'],
      'Cross-Origin-Embedder-Policy': ['require-corp']
    }
  });
});
```

### Without SharedArrayBuffer

If you can't enable COOP/COEP, use `RingBuffer` in the main process:

```javascript
const ring = nVideo.createRingBuffer(16384, 2); // 16k frames, stereo

// In your audio processing loop
const { samples, data } = input.readAudio(4096);
ring.write(data, samples);

// Later, read from ring buffer
const out = new Float32Array(4096 * 2);
const read = ring.read(out, 4096);
```

---

## Buffer Management

### BufferPool

Pre-allocated buffers for zero-GC streaming.

```javascript
const pool = nVideo.createBufferPool({
  audioBufferSize: 4096 * 2,        // 4096 samples * 2 channels
  audioBufferCount: 16,
  videoBufferSize: 1920 * 1080 * 3, // 1080p RGB24
  videoBufferCount: 8
});

// Streaming decode with pool
const input = nVideo.openInput('video.mp4');
input.openVideoStream();

while (input.isOpen()) {
  const width = input.getVideoWidth();
  const height = input.getVideoHeight();

  // Acquire buffer from pool
  const buffer = pool.acquireVideo(width * height * 3);

  // Decode into buffer
  const frame = input.readVideoFrame(buffer);
  if (!frame.width) break;

  // Use buffer...

  // Release back to pool
  pool.releaseVideo(buffer);
}

// Stats
console.log(pool.stats);
// {
//   audioTotal: 16,
//   audioFree: 16,
//   audioAcquired: 0,
//   audioPeak: 4,
//   videoTotal: 8,
//   videoFree: 8,
//   videoAcquired: 0,
//   videoPeak: 2
// }

pool.dispose();
```

### RingBuffer

Pure JS ring buffer for main process.

```javascript
const ring = nVideo.createRingBuffer(16384, 2); // 16k frames, stereo

// Producer
function produceAudio(input) {
  const { samples, data } = input.readAudio(4096);
  if (samples > 0) {
    ring.write(data, samples);
  }
  return samples;
}

// Consumer
function consumeAudio() {
  if (ring.available >= 4096) {
    const out = new Float32Array(4096 * 2);
    const read = ring.read(out, 4096);
    return { data: out, samples: read };
  }
  return null;
}

// Check state
console.log(ring.available); // Frames available
console.log(ring.space);     // Frames that can be written
console.log(ring.empty);     // Boolean
console.log(ring.full);      // Boolean

// Clear
ring.clear();
```

---

## Electron Integration

### Main Process

```javascript
// main.js
const { ipcMain } = require('electron');
const nVideo = require('nvideo');

ipcMain.handle('video:probe', (event, path) => {
  return nVideo.probe(path);
});

ipcMain.handle('video:transcode', async (event, input, output, opts) => {
  return nVideo.transcode(input, output, opts);
});
```

### Renderer Process

```javascript
// renderer.js
const { ipcRenderer } = require('electron');

async function probeFile(path) {
  const info = await ipcRenderer.invoke('video:probe', path);
  console.log(info);
}
```

### Direct Usage in Renderer

For streaming players, you can use nVideo directly in renderer if:
1. Node integration is enabled
2. Context isolation is disabled (or use preload)

```javascript
// preload.js
const nVideo = require('nvideo');

contextBridge.exposeInMainWorld('nVideo', {
  openInput: nVideo.openInput,
  probe: nVideo.probe,
  AudioStreamPlayer: nVideo.AudioStreamPlayer,
  VideoStreamPlayer: nVideo.VideoStreamPlayer
});
```

---

## Performance Tips

### Audio

1. **Match sample rates** - Avoid resampling if possible
2. **Use AudioWorklet** - Lowest latency, most efficient
3. **Buffer size** - 4096 samples is a good balance (93ms @ 44.1kHz)
4. **Reuse buffers** - Use BufferPool for decode loops

### Video

1. **Pre-allocate buffers** - Don't allocate in the decode loop
2. **Use appropriate format** - RGB24 is fastest for canvas
3. **Scale on GPU** - Let the player handle scaling via canvas
4. **Frame dropping** - Let the player manage sync

### Memory

```javascript
// Bad: Allocating in loop
while (true) {
  const buffer = new Uint8Array(width * height * 3);
  input.readVideoFrame(buffer);
}

// Good: Reuse buffer
const buffer = new Uint8Array(width * height * 3);
while (true) {
  input.readVideoFrame(buffer);
}

// Best: Use BufferPool
const pool = nVideo.createBufferPool({ videoBufferCount: 8 });
while (true) {
  const buffer = pool.acquireVideo(width * height * 3);
  input.readVideoFrame(buffer);
  pool.releaseVideo(buffer);
}
```

---

## Troubleshooting

### AudioWorklet Not Supported

Check browser support:

```javascript
if (typeof AudioWorklet === 'undefined') {
  // Fallback to ScriptProcessorNode or different approach
}
```

### SharedArrayBuffer Not Available

```javascript
if (typeof SharedArrayBuffer === 'undefined') {
  // Use RingBuffer instead
  const ring = nVideo.createRingBuffer(16384, 2);
}
```

### Buffer Underruns

Increase buffer duration:

```javascript
const player = new nVideo.AudioStreamPlayer(audioContext, {
  bufferDuration: 5.0  // 5 second buffer
});
```

### Video Out of Sync

The player should auto-sync. If not:
1. Check that audio and video have same duration
2. Verify frame rates match
3. Use `AVStreamPlayer` for automatic sync
