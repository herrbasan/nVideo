'use strict';

// ==================== AudioWorklet Processor Source ====================
// This code is loaded as a blob URL into AudioWorklet.
// It reads from SharedArrayBuffer ring buffer using atomic operations.

const AUDIO_WORKLET_SOURCE = `
const CONTROL = {
  WRITE_PTR: 0,
  READ_PTR: 1,
  STATE: 2,
  SAMPLE_RATE: 3,
  CHANNELS: 4,
  LOOP_ENABLED: 5,
  LOOP_START: 6,
  LOOP_END: 7,
  TOTAL_FRAMES: 8,
  UNDERRUN_COUNT: 9,
  START_TIME_HI: 10,
  START_TIME_LO: 11,
  SIZE: 12
};

const STATE = {
  STOPPED: 0,
  PLAYING: 1,
  PAUSED: 2
};

class NVideoAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.controlBuffer = null;
    this.audioBuffer = null;
    this.ringSize = 0;
    this.channels = 2;
    this.isReady = false;
    this.shouldTerminate = false;
    this.framesPlayed = 0;
    this.hasEnded = false;

    this.port.onmessage = (e) => {
      const d = e.data;
      if (!d || !d.type) return;

      if (d.type === 'init') {
        this.controlBuffer = new Int32Array(d.controlSAB);
        this.audioBuffer = new Float32Array(d.audioSAB);
        this.ringSize = d.ringSize;
        this.channels = d.channels || 2;
        this.isReady = true;
        return;
      }

      if (d.type === 'reset') {
        this.framesPlayed = 0;
        this.hasEnded = false;
        if (this.controlBuffer) {
          Atomics.store(this.controlBuffer, CONTROL.READ_PTR, 0);
        }
        return;
      }

      if (d.type === 'seek') {
        this.framesPlayed = 0;
        this.hasEnded = false;
        if (this.controlBuffer) {
          Atomics.store(this.controlBuffer, CONTROL.READ_PTR, 0);
        }
        return;
      }

      if (d.type === 'dispose') {
        this.shouldTerminate = true;
        return;
      }
    };
  }

  process(inputs, outputs, parameters) {
    if (this.shouldTerminate) return false;

    if (!this.isReady || !this.controlBuffer || !this.audioBuffer) {
      const output = outputs[0];
      if (output) {
        for (let ch = 0; ch < output.length; ch++) {
          output[ch].fill(0);
        }
      }
      return true;
    }

    const output = outputs[0];
    if (!output || output.length === 0) return true;

    const channel0 = output[0];
    const channel1 = output.length > 1 ? output[1] : output[0];
    const blockSize = channel0.length;

    const currentTime = parameters.currentTime !== undefined ? parameters.currentTime : 0;

    const state = Atomics.load(this.controlBuffer, CONTROL.STATE);

    if (state !== STATE.PLAYING) {
      channel0.fill(0);
      channel1.fill(0);
      return true;
    }

    const startTimeHi = Atomics.load(this.controlBuffer, CONTROL.START_TIME_HI);
    const startTimeLo = Atomics.load(this.controlBuffer, CONTROL.START_TIME_LO);
    if (startTimeHi !== 0 || startTimeLo !== 0) {
      const startTimeView = new DataView(new ArrayBuffer(8));
      startTimeView.setInt32(0, startTimeHi, true);
      startTimeView.setInt32(4, startTimeLo, true);
      const scheduledStart = startTimeView.getFloat64(0, true);
      if (scheduledStart > 0 && currentTime < scheduledStart) {
        channel0.fill(0);
        channel1.fill(0);
        return true;
      }
    }

    const writePtr = Atomics.load(this.controlBuffer, CONTROL.WRITE_PTR);
    const readPtr = Atomics.load(this.controlBuffer, CONTROL.READ_PTR);
    const loopEnabled = Atomics.load(this.controlBuffer, CONTROL.LOOP_ENABLED);
    const totalFrames = Atomics.load(this.controlBuffer, CONTROL.TOTAL_FRAMES);

    let available = writePtr - readPtr;
    if (available < 0) available += this.ringSize;

    let framesRead = 0;
    let localReadPtr = readPtr;

    for (let i = 0; i < blockSize; i++) {
      if (framesRead >= available) {
        channel0[i] = 0;
        channel1[i] = 0;
        Atomics.add(this.controlBuffer, CONTROL.UNDERRUN_COUNT, 1);
      } else {
        const bufferIndex = (localReadPtr % this.ringSize) * this.channels;
        channel0[i] = this.audioBuffer[bufferIndex];
        channel1[i] = this.audioBuffer[bufferIndex + 1];
        localReadPtr++;
        framesRead++;
        this.framesPlayed++;
      }
    }

    Atomics.store(this.controlBuffer, CONTROL.READ_PTR, localReadPtr % this.ringSize);

    if (!loopEnabled && totalFrames > 0 && this.framesPlayed >= totalFrames && !this.hasEnded) {
      this.hasEnded = true;
      this.port.postMessage({ type: 'ended' });
    }

    if (this.framesPlayed % 2400 < blockSize) {
      this.port.postMessage({
        type: 'position',
        frames: this.framesPlayed,
        readPtr: localReadPtr
      });
    }

    return true;
  }
}

registerProcessor('nvideo-audio-processor', NVideoAudioProcessor);
`;

// ==================== Control Buffer Layout ====================

const CONTROL = {
  WRITE_PTR: 0,
  READ_PTR: 1,
  STATE: 2,
  SAMPLE_RATE: 3,
  CHANNELS: 4,
  LOOP_ENABLED: 5,
  LOOP_START: 6,
  LOOP_END: 7,
  TOTAL_FRAMES: 8,
  UNDERRUN_COUNT: 9,
  START_TIME_HI: 10,
  START_TIME_LO: 11,
  SIZE: 12
};

const STATE = {
  STOPPED: 0,
  PLAYING: 1,
  PAUSED: 2
};

// ==================== Audio Stream Player ====================

class AudioStreamPlayer {
  constructor(audioContext, opts = {}) {
    this.audioContext = audioContext;
    this.ringSeconds = opts.ringSeconds || 2;
    this.threadCount = opts.threadCount || 0;
    this.connectDestination = opts.connectDestination !== false;

    this.AudioInput = opts.AudioInput || null;
    this.openInput = opts.openInput || null;

    this.decoder = null;
    this.workletNode = null;
    this.gainNode = null;

    this.controlSAB = null;
    this.audioSAB = null;
    this.controlBuffer = null;
    this.audioBuffer = null;
    this.ringSize = 0;

    this.isPlaying = false;
    this.isLoaded = false;
    this.isLoop = false;
    this.filePath = null;
    this.duration = 0;
    this._sampleRate = 44100;
    this._channels = 2;
    this.totalFrames = 0;

    this._framesWritten = 0;
    this._targetFrames = 0;
    this._eof = false;

    this.onEndedCallback = null;
    this.onPositionCallback = null;
    this.workletReady = false;
    this.feedTimer = null;
    this.isDisposed = false;

    this._posMsgAt = 0;
    this._posMsgFrames = 0;
    this._seekOffset = 0;

    this.currentFrames = 0;
    this._underrunFrames = 0;

    this.prebufferSize = opts.prebufferSize || 10;
    this.chunkSeconds = opts.chunkSeconds || 0.10;
    this.chunkFrames = 4096;
    this.feedIntervalMs = opts.feedIntervalMs || 20;
    this._feedNextAtMs = 0;

    this._workletBlobUrl = null;
  }

  async _ensureWorklet() {
    if (this.workletReady) return;

    const blob = new Blob([AUDIO_WORKLET_SOURCE], { type: 'application/javascript' });
    this._workletBlobUrl = URL.createObjectURL(blob);

    try {
      await this.audioContext.audioWorklet.addModule(this._workletBlobUrl);
      this.workletReady = true;
    } catch (e) {
      URL.revokeObjectURL(this._workletBlobUrl);
      this._workletBlobUrl = null;
      throw e;
    }
  }

  async open(filePath, opts = {}) {
    if (this.isDisposed) throw new Error('Player disposed');

    if (!this.AudioInput && !this.openInput) {
      throw new Error('AudioInput or openInput not set. Pass via constructor options.');
    }

    await this._ensureWorklet();

    this.stop(true);

    this._seekOffset = 0;
    this._posMsgAt = 0;
    this._posMsgFrames = 0;

    if (this.openInput) {
      this.decoder = this.openInput(filePath, { sampleRate: this.audioContext.sampleRate, threads: this.threadCount });
    } else {
      const nVideo = require('./index');
      this.decoder = nVideo.openInput(filePath, { sampleRate: this.audioContext.sampleRate, threads: this.threadCount });
    }

    if (!this.decoder || !this.decoder.isOpen()) {
      throw new Error('Failed to open audio file: ' + filePath);
    }

    this.filePath = filePath;
    this._sampleRate = this.decoder.getSampleRate() || this.audioContext.sampleRate;
    this._channels = this.decoder.getChannels() || 2;
    this.duration = this.decoder.getDuration() || 0;
    this.totalFrames = Math.floor(this.duration * this._sampleRate);

    let cf = (this._sampleRate * this.chunkSeconds) | 0;
    cf = (cf + 127) & ~127;
    if (cf < 2048) cf = 2048;
    this.chunkFrames = cf;

    this._framesWritten = 0;
    this._targetFrames = this.totalFrames;
    this._eof = false;

    const neededRingSize = Math.ceil(this.ringSeconds * this._sampleRate);
    const neededAudioBufferSize = neededRingSize * this._channels;

    const needNewSAB = !this.audioSAB ||
                       !this.controlSAB ||
                       this.ringSize !== neededRingSize ||
                       (this.audioSAB.byteLength / 4) !== neededAudioBufferSize;

    if (needNewSAB) {
      this.controlSAB = null;
      this.audioSAB = null;
      this.controlBuffer = null;
      this.audioBuffer = null;

      this.ringSize = neededRingSize;

      this.controlSAB = new SharedArrayBuffer(CONTROL.SIZE * 4);
      this.audioSAB = new SharedArrayBuffer(neededAudioBufferSize * 4);
    } else {
      this.ringSize = neededRingSize;
    }

    this.controlBuffer = new Int32Array(this.controlSAB);
    this.audioBuffer = new Float32Array(this.audioSAB);

    Atomics.store(this.controlBuffer, CONTROL.WRITE_PTR, 0);
    Atomics.store(this.controlBuffer, CONTROL.READ_PTR, 0);
    Atomics.store(this.controlBuffer, CONTROL.STATE, STATE.STOPPED);
    Atomics.store(this.controlBuffer, CONTROL.SAMPLE_RATE, this._sampleRate);
    Atomics.store(this.controlBuffer, CONTROL.CHANNELS, this._channels);
    Atomics.store(this.controlBuffer, CONTROL.LOOP_ENABLED, this.isLoop ? 1 : 0);
    Atomics.store(this.controlBuffer, CONTROL.LOOP_START, 0);
    Atomics.store(this.controlBuffer, CONTROL.LOOP_END, this.totalFrames);
    Atomics.store(this.controlBuffer, CONTROL.TOTAL_FRAMES, this._targetFrames);
    Atomics.store(this.controlBuffer, CONTROL.UNDERRUN_COUNT, 0);
    Atomics.store(this.controlBuffer, CONTROL.START_TIME_HI, 0);
    Atomics.store(this.controlBuffer, CONTROL.START_TIME_LO, 0);

    if (this.gainNode) {
      try { this.gainNode.disconnect(); } catch(e) {}
    }
    this.gainNode = this.audioContext.createGain();
    if (this.connectDestination) {
      this.gainNode.connect(this.audioContext.destination);
    }

    if (this.workletNode && !needNewSAB) {
      this.workletNode.port.postMessage({ type: 'reset' });
      try { this.workletNode.connect(this.gainNode); } catch(e) {}
    } else {
      if (this.workletNode) {
        this.workletNode.port.postMessage({ type: 'dispose' });
        try { this.workletNode.disconnect(); } catch(e) {}
        this.workletNode.port.onmessage = null;
        this.workletNode = null;
      }

      this.workletNode = new AudioWorkletNode(this.audioContext, 'nvideo-audio-processor', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [this._channels]
      });

      this.workletNode.port.onmessage = (event) => {
        const d = event.data;
        if (!d || !d.type) return;

        if (d.type === 'ended') {
          if (!this.isLoop && this.onEndedCallback) {
            this.onEndedCallback();
          }
          return;
        }

        if (d.type === 'position') {
          this._posMsgAt = this.audioContext.currentTime;
          this._posMsgFrames = d.frames | 0;
          this.currentFrames = this._posMsgFrames | 0;
          if (this.onPositionCallback) {
            this.onPositionCallback(this.getCurrentTime(), this.duration);
          }
          return;
        }
      };

      this.workletNode.port.postMessage({
        type: 'init',
        controlSAB: this.controlSAB,
        audioSAB: this.audioSAB,
        ringSize: this.ringSize,
        channels: this._channels
      });

      this.workletNode.connect(this.gainNode);
    }

    this._fillRingBuffer();
    this.isLoaded = true;

    return {
      duration: this.duration,
      sampleRate: this._sampleRate,
      channels: this._channels,
      totalFrames: this.totalFrames
    };
  }

  _getBufferedFrames(writePtr, readPtr) {
    let buffered = writePtr - readPtr;
    if (buffered < 0) buffered += this.ringSize;
    return buffered;
  }

  _updateDiagnosticsFromControl(writePtr, readPtr) {
    this._underrunFrames = Atomics.load(this.controlBuffer, CONTROL.UNDERRUN_COUNT);
  }

  _pump(maxChunks = 4) {
    if (!this.decoder || !this.controlBuffer || !this.audioBuffer || !this.ringSize) return 0;
    let totalRead = 0;

    let pb = (this.prebufferSize | 0);
    if (pb <= 0) pb = 10;
    const cf = this.chunkFrames | 0;
    let low = pb * cf;
    if (low < (cf * 2)) low = cf * 2;
    let high = low + (cf * 2);
    const cap = (this.ringSize - 1) | 0;
    if (high > cap) high = cap;
    if (low > cap) low = cap;

    for (let i = 0; i < maxChunks; i++) {
      const writePtr = Atomics.load(this.controlBuffer, CONTROL.WRITE_PTR) | 0;
      const readPtr = Atomics.load(this.controlBuffer, CONTROL.READ_PTR) | 0;
      const buffered = this._getBufferedFrames(writePtr, readPtr);

      if (buffered >= high) {
        this._updateDiagnosticsFromControl(writePtr, readPtr);
        break;
      }

      const available = this.ringSize - buffered - 1;
      if (available <= 0) {
        this._updateDiagnosticsFromControl(writePtr, readPtr);
        break;
      }

      const framesToRead = Math.min(available, cf);
      const samplesToRead = framesToRead * this._channels;
      const result = this.decoder.readAudio(samplesToRead);

      if (!result || result.samples <= 0) {
        if (this.isLoop) {
          this.decoder.seek(0);
          this._framesWritten = 0;
          this._eof = false;
        } else {
          this._eof = true;
          Atomics.store(this.controlBuffer, CONTROL.TOTAL_FRAMES, this._framesWritten | 0);
        }
        this._updateDiagnosticsFromControl(writePtr, readPtr);
        break;
      }

      const framesRead = Math.floor((result.samples | 0) / this._channels);
      if (framesRead <= 0) break;

      this._framesWritten += framesRead;

      const samplesRead = framesRead * this._channels;
      const src = result.data;
      const srcView = src.subarray ? src.subarray(0, samplesRead) : src;

      const writeFrame = writePtr;
      const dst0 = writeFrame * this._channels;
      const framesToEnd = this.ringSize - writeFrame;
      const samplesToEnd = framesToEnd * this._channels;

      if (samplesRead <= samplesToEnd) {
        this.audioBuffer.set(srcView, dst0);
      } else {
        this.audioBuffer.set(srcView.subarray(0, samplesToEnd), dst0);
        this.audioBuffer.set(srcView.subarray(samplesToEnd, samplesRead), 0);
      }

      const newWrite = (writeFrame + framesRead) % this.ringSize;
      Atomics.store(this.controlBuffer, CONTROL.WRITE_PTR, newWrite);

      totalRead += framesRead;
      if (buffered + framesRead >= low) break;
    }

    return totalRead;
  }

  _fillRingBuffer() {
    if (!this.decoder || !this.controlBuffer || !this.audioBuffer) return 0;
    return this._pump(1);
  }

  _setScheduledStart(when) {
    if (!this.controlBuffer) return;
    const view = new DataView(new ArrayBuffer(8));
    view.setFloat64(0, when, true);
    Atomics.store(this.controlBuffer, CONTROL.START_TIME_HI, view.getInt32(0, true));
    Atomics.store(this.controlBuffer, CONTROL.START_TIME_LO, view.getInt32(4, true));
  }

  _startFeedLoop() {
    if (this.isDisposed || !this.isPlaying) return;

    this._pump(4);

    const interval = (this.feedIntervalMs | 0) > 0 ? (this.feedIntervalMs | 0) : 20;
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

  async play(when = 0) {
    if (this.isDisposed || !this.isLoaded) return;

    if (this.audioContext.state === 'suspended') {
      await this.audioContext.resume();
    }

    if (this.workletNode && this.gainNode) {
      try { this.workletNode.connect(this.gainNode); } catch(e) {}
    }

    this._setScheduledStart(when);
    Atomics.store(this.controlBuffer, CONTROL.STATE, STATE.PLAYING);

    if (this.isPlaying) return;

    this.isPlaying = true;
    this._feedNextAtMs = 0;
    this._startFeedLoop();
  }

  pause() {
    this.isPlaying = false;

    if (this.feedTimer) {
      clearTimeout(this.feedTimer);
      this.feedTimer = null;
    }

    if (this.controlBuffer) {
      Atomics.store(this.controlBuffer, CONTROL.STATE, STATE.PAUSED);
    }

    if (this.workletNode) {
      try { this.workletNode.disconnect(); } catch(e) {}
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

    if (this.decoder) {
      try { this.decoder.close(); } catch(e) {}
      this.decoder = null;
    }

    if (this.workletNode) {
      try {
        this.workletNode.port.postMessage({ type: 'dispose' });
        this.workletNode.disconnect();
      } catch(e) {}
      this.workletNode.port.onmessage = null;
      this.workletNode = null;
    }

    if (this.gainNode) {
      try { this.gainNode.disconnect(); } catch(e) {}
      this.gainNode = null;
    }

    if (!keepSAB) {
      this.controlSAB = null;
      this.audioSAB = null;
      this.controlBuffer = null;
      this.audioBuffer = null;
      this.ringSize = 0;
    }

    this._framesWritten = 0;
    this._targetFrames = 0;
    this._eof = false;
    this._posMsgAt = 0;
    this._posMsgFrames = 0;
    this._seekOffset = 0;
    this.currentFrames = 0;
  }

  seek(seconds) {
    if (!this.decoder || !this.controlBuffer) return false;

    const success = this.decoder.seek(seconds);
    if (success) {
      Atomics.store(this.controlBuffer, CONTROL.WRITE_PTR, 0);
      Atomics.store(this.controlBuffer, CONTROL.READ_PTR, 0);

      this._seekOffset = seconds;
      this._posMsgAt = 0;
      this._posMsgFrames = 0;

      this._framesWritten = 0;
      this._eof = false;
      this._targetFrames = Math.max(0, (this.totalFrames | 0) - Math.floor(seconds * this._sampleRate));
      Atomics.store(this.controlBuffer, CONTROL.TOTAL_FRAMES, this._targetFrames | 0);

      if (this.workletNode) {
        this.workletNode.port.postMessage({ type: 'seek', offsetFrames: Math.floor(seconds * this._sampleRate) });
      }

      for (let i = 0; i < 16; i++) {
        const read = this._fillRingBuffer();
        if (read <= 0) break;
      }
    }
    return success;
  }

  getCurrentTime() {
    if (!this.controlBuffer || !this._sampleRate) return 0;

    let frames = this._posMsgFrames | 0;

    if (this.isPlaying && this.audioContext && isFinite(this.audioContext.currentTime)) {
      const tNow = this.audioContext.currentTime;
      const tMsg = this._posMsgAt;
      const dt = tNow - tMsg;
      if (tMsg > 0 && dt > 0 && dt < 0.20) {
        frames = (this._posMsgFrames | 0) + Math.floor(dt * this._sampleRate);
      }
    }

    if (this.duration > 0) {
      const totalFrames = Math.floor(this.duration * this._sampleRate);
      if (totalFrames > 0 && frames >= totalFrames) {
        frames = frames % totalFrames;
      }
    }

    const time = this._seekOffset + (frames / this._sampleRate);
    return Math.min(time, this.duration);
  }

  setLoop(enabled) {
    this.isLoop = enabled;
    if (this.controlBuffer) {
      Atomics.store(this.controlBuffer, CONTROL.LOOP_ENABLED, enabled ? 1 : 0);
    }
  }

  setVolume(vol) {
    if (this.gainNode) {
      this.gainNode.gain.value = vol;
    }
  }

  onEnded(callback) {
    this.onEndedCallback = callback;
  }

  onPosition(callback) {
    this.onPositionCallback = callback;
  }

  getUnderrunCount() {
    if (!this.controlBuffer) return 0;
    return Atomics.load(this.controlBuffer, CONTROL.UNDERRUN_COUNT);
  }

  getBufferedFrames() {
    if (!this.controlBuffer || !this.ringSize) return 0;
    const writePtr = Atomics.load(this.controlBuffer, CONTROL.WRITE_PTR);
    const readPtr = Atomics.load(this.controlBuffer, CONTROL.READ_PTR);
    return this._getBufferedFrames(writePtr, readPtr);
  }

  dispose() {
    this.stop(false);
    this.isDisposed = true;
    if (this._workletBlobUrl) {
      URL.revokeObjectURL(this._workletBlobUrl);
      this._workletBlobUrl = null;
    }
  }
}

module.exports = { AudioStreamPlayer, AUDIO_WORKLET_SOURCE };
