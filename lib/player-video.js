'use strict';

// ==================== Video Frame Queue ====================

class VideoFrameQueue {
  constructor(maxFrames = 8) {
    this.maxFrames = maxFrames;
    this.frames = [];
    this.writePos = 0;
    this.readPos = 0;
    this.count = 0;
  }

  push(frame) {
    if (this.count >= this.maxFrames) {
      const old = this.frames[this.readPos];
      if (old) old.close();
      this.readPos = (this.readPos + 1) % this.maxFrames;
      this.count--;
    }

    this.frames[this.writePos] = frame;
    this.writePos = (this.writePos + 1) % this.maxFrames;
    this.count++;
  }

  pop() {
    if (this.count === 0) return null;
    const frame = this.frames[this.readPos];
    this.frames[this.readPos] = null;
    this.readPos = (this.readPos + 1) % this.maxFrames;
    this.count--;
    return frame;
  }

  clear() {
    for (let i = 0; i < this.frames.length; i++) {
      if (this.frames[i]) {
        this.frames[i].close();
        this.frames[i] = null;
      }
    }
    this.writePos = 0;
    this.readPos = 0;
    this.count = 0;
  }

  get length() {
    return this.count;
  }

  get empty() {
    return this.count === 0;
  }

  get full() {
    return this.count >= this.maxFrames;
  }
}

// ==================== Control Buffer for Video ====================

const VIDEO_CONTROL = {
  WRITE_PTR: 0,
  READ_PTR: 1,
  STATE: 2,
  WIDTH: 3,
  HEIGHT: 4,
  FORMAT: 5,
  STRIDE: 6,
  FRAME_PTS_HI: 7,
  FRAME_PTS_LO: 8,
  FRAME_NUM: 9,
  KEYFRAME: 10,
  TOTAL_FRAMES: 11,
  UNDERRUN_COUNT: 12,
  SIZE: 16
};

const VIDEO_STATE = {
  STOPPED: 0,
  PLAYING: 1,
  PAUSED: 2
};

// ==================== Video Stream Player ====================

class VideoStreamPlayer {
  constructor(opts = {}) {
    this.canvas = opts.canvas || null;
    this.ctx = null;

    this.openInput = opts.openInput || null;
    this.AudioInput = opts.AudioInput || null;

    this.decoder = null;

    this.frameQueue = new VideoFrameQueue(opts.maxQueuedFrames || 8);

    this.controlSAB = null;
    this.videoSAB = null;
    this.controlBuffer = null;
    this.videoBuffer = null;
    this.ringSize = 0;

    this.isPlaying = false;
    this.isLoaded = false;
    this.isLoop = false;
    this.filePath = null;
    this.duration = 0;
    this._fps = 30;
    this._width = 0;
    this._height = 0;
    this._format = 'rgb24';
    this._stride = 0;
    this.totalFrames = 0;

    this._framesWritten = 0;
    this._targetFrames = 0;
    this._eof = false;

    this.onEndedCallback = null;
    this.onFrameCallback = null;
    this.onPositionCallback = null;
    this.feedTimer = null;
    this.renderTimer = null;
    this.isDisposed = false;

    this._posMsgAt = 0;
    this._posMsgFrames = 0;
    this._seekOffset = 0;

    this.currentFrameNum = 0;
    this._underrunCount = 0;

    this.prebufferSize = opts.prebufferSize || 3;
    this.feedIntervalMs = opts.feedIntervalMs || 16;
    this._feedNextAtMs = 0;

    this._targetWidth = opts.width || 0;
    this._targetHeight = opts.height || 0;
    this._targetFormat = opts.format || 'rgb24';

    this._frameBytes = 0;
    this._ringFrameCapacity = 0;
  }

  async open(filePath, opts = {}) {
    if (this.isDisposed) throw new Error('Player disposed');

    if (!this.openInput && !this.AudioInput) {
      throw new Error('openInput or AudioInput not set. Pass via constructor options.');
    }

    this.stop(true);

    this._seekOffset = 0;
    this._posMsgAt = 0;
    this._posMsgFrames = 0;

    if (this.openInput) {
      this.decoder = this.openInput(filePath, { threads: opts.threads || 0 });
    } else {
      const nVideo = require('./index');
      this.decoder = nVideo.openInput(filePath, { threads: opts.threads || 0 });
    }

    if (!this.decoder || !this.decoder.isOpen()) {
      throw new Error('Failed to open video file: ' + filePath);
    }

    this.decoder.openVideoStream();

    this.filePath = filePath;
    this._width = this.decoder.getVideoWidth();
    this._height = this.decoder.getVideoHeight();
    this._fps = this.decoder.getVideoFPS() || 30;
    this.duration = this.decoder.getDuration() || 0;

    if (this._targetWidth > 0 && this._targetHeight > 0) {
      this._width = this._targetWidth;
      this._height = this._targetHeight;
    }

    this._width = (this._width + 1) & ~1;
    this._height = (this._height + 1) & ~1;

    this._format = this._targetFormat;
    this._stride = this._width * 3;
    this._frameBytes = this._width * this._height * 3;

    this.totalFrames = Math.floor(this.duration * this._fps);

    this._framesWritten = 0;
    this._targetFrames = this.totalFrames;
    this._eof = false;

    this._ringFrameCapacity = opts.ringFrameCapacity || 30;
    const neededVideoBufferSize = this._ringFrameCapacity * this._frameBytes;

    const needNewSAB = !this.videoSAB ||
                       !this.controlSAB ||
                       (this.videoSAB.byteLength) !== neededVideoBufferSize;

    if (needNewSAB) {
      this.controlSAB = null;
      this.videoSAB = null;
      this.controlBuffer = null;
      this.videoBuffer = null;

      this.controlSAB = new SharedArrayBuffer(VIDEO_CONTROL.SIZE * 4);
      this.videoSAB = new SharedArrayBuffer(neededVideoBufferSize);
    }

    this.controlBuffer = new Int32Array(this.controlSAB);
    this.videoBuffer = new Uint8Array(this.videoSAB);

    this.ringSize = this._ringFrameCapacity;

    Atomics.store(this.controlBuffer, VIDEO_CONTROL.WRITE_PTR, 0);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.READ_PTR, 0);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.STATE, VIDEO_STATE.STOPPED);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.WIDTH, this._width);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.HEIGHT, this._height);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.FORMAT, 0);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.STRIDE, this._stride);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.FRAME_PTS_HI, 0);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.FRAME_PTS_LO, 0);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.FRAME_NUM, 0);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.KEYFRAME, 0);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.TOTAL_FRAMES, this._targetFrames);
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.UNDERRUN_COUNT, 0);

    if (this.canvas) {
      this.canvas.width = this._width;
      this.canvas.height = this._height;
      this.ctx = this.canvas.getContext('2d');
    }

    this._fillBuffer();
    this.isLoaded = true;

    return {
      duration: this.duration,
      width: this._width,
      height: this._height,
      fps: this._fps,
      totalFrames: this.totalFrames
    };
  }

  _getBufferedFrames(writePtr, readPtr) {
    let buffered = writePtr - readPtr;
    if (buffered < 0) buffered += this.ringSize;
    return buffered;
  }

  _pump(maxFrames = 4) {
    if (!this.decoder || !this.controlBuffer || !this.videoBuffer || !this.ringSize) return 0;
    let totalRead = 0;

    const low = Math.min(this.prebufferSize, this.ringSize - 1);

    for (let i = 0; i < maxFrames; i++) {
      const writePtr = Atomics.load(this.controlBuffer, VIDEO_CONTROL.WRITE_PTR) | 0;
      const readPtr = Atomics.load(this.controlBuffer, VIDEO_CONTROL.READ_PTR) | 0;
      const buffered = this._getBufferedFrames(writePtr, readPtr);

      if (buffered >= low) break;

      const writeFrame = writePtr;
      const dstOffset = writeFrame * this._frameBytes;

      const frameResult = this.decoder.readVideoFrame(
        this.videoBuffer.subarray(dstOffset, dstOffset + this._frameBytes),
        { width: this._width, height: this._height, format: this._format }
      );

      if (!frameResult || frameResult.width === 0) {
        if (this.isLoop) {
          this.decoder.seek(0);
          this._framesWritten = 0;
          this._eof = false;
        } else {
          this._eof = true;
          Atomics.store(this.controlBuffer, VIDEO_CONTROL.TOTAL_FRAMES, this._framesWritten | 0);
        }
        break;
      }

      this._framesWritten++;

      const ptsHi = Math.floor(frameResult.pts) | 0;
      const ptsLo = ((frameResult.pts - ptsHi) * 1000000) | 0;
      Atomics.store(this.controlBuffer, VIDEO_CONTROL.FRAME_PTS_HI, ptsHi);
      Atomics.store(this.controlBuffer, VIDEO_CONTROL.FRAME_PTS_LO, ptsLo);
      Atomics.store(this.controlBuffer, VIDEO_CONTROL.FRAME_NUM, frameResult.frameNum | 0);
      Atomics.store(this.controlBuffer, VIDEO_CONTROL.KEYFRAME, frameResult.keyframe ? 1 : 0);

      const newWrite = (writeFrame + 1) % this.ringSize;
      Atomics.store(this.controlBuffer, VIDEO_CONTROL.WRITE_PTR, newWrite);

      totalRead++;
    }

    return totalRead;
  }

  _fillBuffer() {
    if (!this.decoder || !this.controlBuffer || !this.videoBuffer) return 0;
    return this._pump(this.prebufferSize);
  }

  _startFeedLoop() {
    if (this.isDisposed || !this.isPlaying) return;

    this._pump(4);

    const interval = (this.feedIntervalMs | 0) > 0 ? (this.feedIntervalMs | 0) : 16;
    const now = (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
    if (!this._feedNextAtMs || (now - this._feedNextAtMs) > (interval * 10)) {
      this._feedNextAtMs = now + interval;
    } else {
      this._feedNextAtMs += interval;
    }
    let delay = this._feedNextAtMs - now;
    if (delay < 0) delay = 0;
    this.feedTimer = setTimeout(() => this._startFeedLoop(), delay);
  }

  _startRenderLoop() {
    if (this.isDisposed || !this.isPlaying) return;

    this._renderFrame();

    const frameInterval = 1000 / this._fps;
    this.renderTimer = setTimeout(() => this._startRenderLoop(), frameInterval);
  }

  _renderFrame() {
    if (!this.ctx || !this.controlBuffer || !this.videoBuffer) return;

    const writePtr = Atomics.load(this.controlBuffer, VIDEO_CONTROL.WRITE_PTR);
    const readPtr = Atomics.load(this.controlBuffer, VIDEO_CONTROL.READ_PTR);

    let available = writePtr - readPtr;
    if (available < 0) available += this.ringSize;

    if (available === 0) {
      this._underrunCount++;
      Atomics.add(this.controlBuffer, VIDEO_CONTROL.UNDERRUN_COUNT, 1);
      return;
    }

    const readFrame = readPtr;
    const srcOffset = readFrame * this._frameBytes;

    const imageData = this.ctx.createImageData(this._width, this._height);
    const dst = imageData.data;
    const src = this.videoBuffer;

    for (let y = 0; y < this._height; y++) {
      const srcRow = srcOffset + y * this._stride;
      const dstRow = y * this._width * 4;
      for (let x = 0; x < this._width; x++) {
        const si = srcRow + x * 3;
        const di = dstRow + x * 4;
        dst[di] = src[si];
        dst[di + 1] = src[si + 1];
        dst[di + 2] = src[si + 2];
        dst[di + 3] = 255;
      }
    }

    this.ctx.putImageData(imageData, 0, 0);

    const newRead = (readPtr + 1) % this.ringSize;
    Atomics.store(this.controlBuffer, VIDEO_CONTROL.READ_PTR, newRead);

    this.currentFrameNum = Atomics.load(this.controlBuffer, VIDEO_CONTROL.FRAME_NUM);
    this._posMsgFrames = this.currentFrameNum;
    this._posMsgAt = (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();

    if (this.onFrameCallback) {
      const ptsHi = Atomics.load(this.controlBuffer, VIDEO_CONTROL.FRAME_PTS_HI);
      const ptsLo = Atomics.load(this.controlBuffer, VIDEO_CONTROL.FRAME_PTS_LO);
      const pts = ptsHi + (ptsLo / 1000000);
      const keyframe = Atomics.load(this.controlBuffer, VIDEO_CONTROL.KEYFRAME) !== 0;
      this.onFrameCallback({
        frameNum: this.currentFrameNum,
        pts: pts,
        keyframe: keyframe,
        buffered: available - 1
      });
    }

    if (!this.isLoop && this._framesWritten > 0 && this.currentFrameNum >= this._framesWritten - 1) {
      if (this.onEndedCallback) {
        this.onEndedCallback();
      }
    }
  }

  play() {
    if (this.isDisposed || !this.isLoaded) return;

    Atomics.store(this.controlBuffer, VIDEO_CONTROL.STATE, VIDEO_STATE.PLAYING);

    if (this.isPlaying) return;

    this.isPlaying = true;
    this._feedNextAtMs = 0;
    this._startFeedLoop();
    this._startRenderLoop();
  }

  pause() {
    this.isPlaying = false;

    if (this.feedTimer) {
      clearTimeout(this.feedTimer);
      this.feedTimer = null;
    }

    if (this.renderTimer) {
      clearTimeout(this.renderTimer);
      this.renderTimer = null;
    }

    if (this.controlBuffer) {
      Atomics.store(this.controlBuffer, VIDEO_CONTROL.STATE, VIDEO_STATE.PAUSED);
    }
  }

  resume() {
    this.play();
  }

  stop(keepSAB = false) {
    this.isPlaying = false;
    this.isLoaded = false;

    if (this.feedTimer) {
      clearTimeout(this.feedTimer);
      this.feedTimer = null;
    }

    if (this.renderTimer) {
      clearTimeout(this.renderTimer);
      this.renderTimer = null;
    }

    if (this.decoder) {
      try { this.decoder.close(); } catch(e) {}
      this.decoder = null;
    }

    this.frameQueue.clear();

    if (!keepSAB) {
      this.controlSAB = null;
      this.videoSAB = null;
      this.controlBuffer = null;
      this.videoBuffer = null;
      this.ringSize = 0;
    }

    this._framesWritten = 0;
    this._targetFrames = 0;
    this._eof = false;
    this._posMsgAt = 0;
    this._posMsgFrames = 0;
    this._seekOffset = 0;
    this.currentFrameNum = 0;

    if (this.ctx) {
      this.ctx.clearRect(0, 0, this._width, this._height);
    }
  }

  seek(seconds) {
    if (!this.decoder || !this.controlBuffer) return false;

    const success = this.decoder.seek(seconds);
    if (success) {
      Atomics.store(this.controlBuffer, VIDEO_CONTROL.WRITE_PTR, 0);
      Atomics.store(this.controlBuffer, VIDEO_CONTROL.READ_PTR, 0);

      this._seekOffset = seconds;
      this._posMsgAt = 0;
      this._posMsgFrames = 0;

      this._framesWritten = 0;
      this._eof = false;
      this._targetFrames = Math.max(0, (this.totalFrames | 0) - Math.floor(seconds * this._fps));
      Atomics.store(this.controlBuffer, VIDEO_CONTROL.TOTAL_FRAMES, this._targetFrames | 0);

      this.frameQueue.clear();

      for (let i = 0; i < 8; i++) {
        const read = this._fillBuffer();
        if (read <= 0) break;
      }
    }
    return success;
  }

  getCurrentTime() {
    if (!this._fps) return 0;

    let frames = this._posMsgFrames | 0;

    if (this.isPlaying) {
      const now = (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
      const dt = (now - this._posMsgAt) / 1000;
      if (this._posMsgAt > 0 && dt > 0 && dt < 0.20) {
        frames = (this._posMsgFrames | 0) + Math.floor(dt * this._fps);
      }
    }

    const time = this._seekOffset + (frames / this._fps);
    return Math.min(time, this.duration);
  }

  setLoop(enabled) {
    this.isLoop = enabled;
  }

  onEnded(callback) {
    this.onEndedCallback = callback;
  }

  onFrame(callback) {
    this.onFrameCallback = callback;
  }

  onPosition(callback) {
    this.onPositionCallback = callback;
  }

  getUnderrunCount() {
    if (!this.controlBuffer) return 0;
    return Atomics.load(this.controlBuffer, VIDEO_CONTROL.UNDERRUN_COUNT);
  }

  getBufferedFrames() {
    if (!this.controlBuffer || !this.ringSize) return 0;
    const writePtr = Atomics.load(this.controlBuffer, VIDEO_CONTROL.WRITE_PTR);
    const readPtr = Atomics.load(this.controlBuffer, VIDEO_CONTROL.READ_PTR);
    return this._getBufferedFrames(writePtr, readPtr);
  }

  dispose() {
    this.stop(false);
    this.isDisposed = true;
  }
}

module.exports = { VideoStreamPlayer, VideoFrameQueue };
