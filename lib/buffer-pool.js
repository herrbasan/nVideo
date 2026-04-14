'use strict';

// ==================== Buffer Pool ====================
// Pre-allocated buffers for zero-GC streaming decode loops.
// Acquire a buffer, use it, release it back to the pool.

class BufferPool {
  constructor(opts = {}) {
    this.audioBufferSize = opts.audioBufferSize || 4096 * 2;
    this.audioBufferCount = opts.audioBufferCount || 8;
    this.videoBufferSize = opts.videoBufferSize || 3840 * 2160 * 3;
    this.videoBufferCount = opts.videoBufferCount || 8;

    this._audioBuffers = [];
    this._audioFree = [];
    this._videoBuffers = [];
    this._videoFree = [];

    for (let i = 0; i < this.audioBufferCount; i++) {
      const buf = new Float32Array(this.audioBufferSize);
      this._audioBuffers.push(buf);
      this._audioFree.push(buf);
    }

    for (let i = 0; i < this.videoBufferCount; i++) {
      const buf = new Uint8Array(this.videoBufferSize);
      this._videoBuffers.push(buf);
      this._videoFree.push(buf);
    }

    this._audioAcquired = 0;
    this._videoAcquired = 0;
    this._audioPeak = 0;
    this._videoPeak = 0;
  }

  acquireAudio(size) {
    if (size > this.audioBufferSize) {
      const buf = new Float32Array(size);
      this._audioAcquired++;
      if (this._audioAcquired > this._audioPeak) this._audioPeak = this._audioAcquired;
      return buf;
    }

    if (this._audioFree.length === 0) {
      const buf = new Float32Array(this.audioBufferSize);
      this._audioBuffers.push(buf);
      this._audioAcquired++;
      if (this._audioAcquired > this._audioPeak) this._audioPeak = this._audioAcquired;
      return buf;
    }

    this._audioAcquired++;
    if (this._audioAcquired > this._audioPeak) this._audioPeak = this._audioAcquired;
    return this._audioFree.pop();
  }

  releaseAudio(buf) {
    if (!buf) return;
    this._audioAcquired--;
    if (this._audioFree.length < this.audioBufferCount) {
      this._audioFree.push(buf);
    }
  }

  acquireVideo(size) {
    if (size > this.videoBufferSize) {
      const buf = new Uint8Array(size);
      this._videoAcquired++;
      if (this._videoAcquired > this._videoPeak) this._videoPeak = this._videoAcquired;
      return buf;
    }

    if (this._videoFree.length === 0) {
      const buf = new Uint8Array(this.videoBufferSize);
      this._videoBuffers.push(buf);
      this._videoAcquired++;
      if (this._videoAcquired > this._videoPeak) this._videoPeak = this._videoAcquired;
      return buf;
    }

    this._videoAcquired++;
    if (this._videoAcquired > this._videoPeak) this._videoPeak = this._videoAcquired;
    return this._videoFree.pop();
  }

  releaseVideo(buf) {
    if (!buf) return;
    this._videoAcquired--;
    if (this._videoFree.length < this.videoBufferCount) {
      this._videoFree.push(buf);
    }
  }

  get stats() {
    return {
      audioTotal: this._audioBuffers.length,
      audioFree: this._audioFree.length,
      audioAcquired: this._audioAcquired,
      audioPeak: this._audioPeak,
      videoTotal: this._videoBuffers.length,
      videoFree: this._videoFree.length,
      videoAcquired: this._videoAcquired,
      videoPeak: this._videoPeak
    };
  }

  dispose() {
    this._audioBuffers.length = 0;
    this._audioFree.length = 0;
    this._videoBuffers.length = 0;
    this._videoFree.length = 0;
  }
}

// ==================== Ring Buffer (pure JS, no SAB) ====================
// For use in Node.js main process where SAB is not needed.
// Single-producer, single-consumer ring buffer.

class RingBuffer {
  constructor(capacity, channels = 2) {
    this.capacity = capacity;
    this.channels = channels;
    this.buffer = new Float32Array(capacity * channels);
    this.writePos = 0;
    this.readPos = 0;
    this.count = 0;
  }

  get available() {
    return this.count;
  }

  get space() {
    return this.capacity - this.count;
  }

  get empty() {
    return this.count === 0;
  }

  get full() {
    return this.count >= this.capacity;
  }

  write(src, frames) {
    if (frames <= 0) return 0;
    const samples = frames * this.channels;
    const toWrite = Math.min(samples, this.space * this.channels);

    const writeSample = this.writePos * this.channels;
    const samplesToEnd = (this.capacity * this.channels) - writeSample;

    if (toWrite <= samplesToEnd) {
      this.buffer.set(src.subarray(0, toWrite), writeSample);
    } else {
      this.buffer.set(src.subarray(0, samplesToEnd), writeSample);
      this.buffer.set(src.subarray(samplesToEnd, toWrite), 0);
    }

    this.writePos = ((this.writePos * this.channels + toWrite) / this.channels) | 0;
    this.writePos = this.writePos % this.capacity;
    this.count += (toWrite / this.channels) | 0;

    return toWrite / this.channels;
  }

  read(dst, frames) {
    if (frames <= 0) return 0;
    const samples = frames * this.channels;
    const toRead = Math.min(samples, this.count * this.channels);

    const readSample = this.readPos * this.channels;
    const samplesToEnd = (this.capacity * this.channels) - readSample;

    if (toRead <= samplesToEnd) {
      dst.set(this.buffer.subarray(readSample, readSample + toRead), 0);
    } else {
      dst.set(this.buffer.subarray(readSample, readSample + samplesToEnd), 0);
      dst.set(this.buffer.subarray(0, toRead - samplesToEnd), samplesToEnd);
    }

    this.readPos = ((this.readPos * this.channels + toRead) / this.channels) | 0;
    this.readPos = this.readPos % this.capacity;
    this.count -= (toRead / this.channels) | 0;

    return toRead / this.channels;
  }

  clear() {
    this.writePos = 0;
    this.readPos = 0;
    this.count = 0;
  }
}

// ==================== Synchronized A/V Player ====================
// Combines AudioStreamPlayer and VideoStreamPlayer with shared clock.

class AVStreamPlayer {
  constructor(opts = {}) {
    this.audioPlayer = null;
    this.videoPlayer = null;
    this.canvas = opts.canvas || null;
    this.audioContext = opts.audioContext || null;

    this.openInput = opts.openInput || null;
    this.isLoop = false;

    this._audioReady = false;
    this._videoReady = false;
    this._onEndedCallback = null;
  }

  async openAudio(filePath, opts = {}) {
    if (!this.audioContext) {
      throw new Error('audioContext not set');
    }

    const { AudioStreamPlayer } = require('./player-audio');
    this.audioPlayer = new AudioStreamPlayer(this.audioContext, {
      openInput: this.openInput,
      ringSeconds: opts.audioRingSeconds || 2,
      connectDestination: opts.audioConnectDestination !== false
    });

    const info = await this.audioPlayer.open(filePath, opts);
    this._audioReady = true;

    this.audioPlayer.onEnded(() => {
      if (!this.isLoop) {
        this._checkBothEnded();
      }
    });

    return info;
  }

  async openVideo(filePath, opts = {}) {
    const { VideoStreamPlayer } = require('./player-video');
    this.videoPlayer = new VideoStreamPlayer({
      openInput: this.openInput,
      canvas: this.canvas,
      width: opts.width || 0,
      height: opts.height || 0,
      ringFrameCapacity: opts.ringFrameCapacity || 30
    });

    const info = await this.videoPlayer.open(filePath, opts);
    this._videoReady = true;

    this.videoPlayer.onEnded(() => {
      if (!this.isLoop) {
        this._checkBothEnded();
      }
    });

    return info;
  }

  _checkBothEnded() {
    const audioEnded = !this.audioPlayer || !this.audioPlayer.isPlaying;
    const videoEnded = !this.videoPlayer || !this.videoPlayer.isPlaying;

    if (audioEnded && videoEnded && this._onEndedCallback) {
      this._onEndedCallback();
    }
  }

  play() {
    if (this.audioPlayer) this.audioPlayer.play();
    if (this.videoPlayer) this.videoPlayer.play();
  }

  pause() {
    if (this.audioPlayer) this.audioPlayer.pause();
    if (this.videoPlayer) this.videoPlayer.pause();
  }

  resume() {
    if (this.audioPlayer) this.audioPlayer.resume();
    if (this.videoPlayer) this.videoPlayer.resume();
  }

  seek(seconds) {
    let success = true;
    if (this.audioPlayer) success = this.audioPlayer.seek(seconds) && success;
    if (this.videoPlayer) success = this.videoPlayer.seek(seconds) && success;
    return success;
  }

  stop() {
    if (this.audioPlayer) this.audioPlayer.stop();
    if (this.videoPlayer) this.videoPlayer.stop();
    this._audioReady = false;
    this._videoReady = false;
  }

  setLoop(enabled) {
    this.isLoop = enabled;
    if (this.audioPlayer) this.audioPlayer.setLoop(enabled);
    if (this.videoPlayer) this.videoPlayer.setLoop(enabled);
  }

  getCurrentTime() {
    if (this.videoPlayer) return this.videoPlayer.getCurrentTime();
    if (this.audioPlayer) return this.audioPlayer.getCurrentTime();
    return 0;
  }

  onEnded(callback) {
    this._onEndedCallback = callback;
  }

  dispose() {
    this.stop();
    if (this.audioPlayer) this.audioPlayer.dispose();
    if (this.videoPlayer) this.videoPlayer.dispose();
  }
}

module.exports = { BufferPool, RingBuffer, AVStreamPlayer };
