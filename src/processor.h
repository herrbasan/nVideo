#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <map>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
    #include <libavutil/avutil.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/channel_layout.h>
    #include <libavutil/opt.h>
}

struct StreamInfo {
    int index;
    std::string type;       // "video", "audio", "subtitle", "data"
    std::string codec;      // short codec name (e.g., "h264", "aac")
    std::string codecLong;  // long codec name
    int width;              // video only
    int height;             // video only
    double fps;             // video only
    int bitrate;            // bits/sec
    std::string pixelFormat;// video only
    int sampleRate;         // audio only
    int channels;           // audio only
    std::string channelLayout; // audio only
    int bitsPerSample;      // audio only
    std::map<std::string, std::string> tags;
};

struct ProbeResult {
    std::string formatName;     // e.g., "mov,mp4"
    std::string formatLong;     // long format name
    int64_t duration;           // microseconds
    int64_t size;              // bytes
    int bitrate;               // bits/sec
    std::map<std::string, std::string> tags;
    std::vector<StreamInfo> streams;
};

struct ThumbnailResult {
    uint8_t* data;             // RGB24 pixel data (owned by result, must be freed)
    int width;                 // output width
    int height;                // output height
    double pts;                // presentation timestamp of the decoded frame
    bool keyframe;             // whether the frame was a keyframe

    ThumbnailResult() : data(nullptr), width(0), height(0), pts(0.0), keyframe(false) {}
    ~ThumbnailResult() { delete[] data; }
};

// Waveform result - peak amplitudes for L/R channels
struct WaveformResult {
    std::vector<float> peaksL;
    std::vector<float> peaksR;
    int points;
    double duration;

    WaveformResult() : points(0), duration(0.0) {}
};

// Progress callback for streaming waveform
typedef std::function<bool(const WaveformResult&, float)> WaveformProgressCallback;

// Transcode progress callback
struct TranscodeProgress {
    double time;              // Current timestamp processed (seconds)
    double percent;            // Progress 0-100
    double speed;              // Processing speed (1.0 = realtime)
    int64_t bitrate;          // Current output bitrate (bits/sec)
    int64_t size;             // Current output file size (bytes)
    int64_t frames;           // Video frames encoded
    double fps;                // Current encode throughput (frames/sec)
    int64_t audioFrames;      // Audio samples encoded
    double audioTime;         // Audio timestamp processed (seconds)
    double estimatedDuration; // Total input duration (seconds)
    int64_t estimatedSize;   // Estimated final file size (bytes)
    double eta;               // Estimated seconds remaining
    int dupFrames;            // Duplicate frames
    int dropFrames;           // Dropped frames
};

typedef std::function<void(const TranscodeProgress&)> TranscodeProgressCallback;

// Transcode options
struct TranscodeOptions {
    // Video options (null = copy stream, empty object = disable)
    struct VideoOpts {
        std::string codec;       // e.g., "libx264", "copy"
        int width = 0;           // 0 = keep original
        int height = 0;          // 0 = keep original
        int crf = -1;            // -1 = keep original or default
        std::string preset;      // e.g., "medium", "fast"
        std::string pixelFormat; // e.g., "yuv420p"
        int64_t bitrate = 0;     // 0 = keep original or default
        int fps = 0;             // 0 = keep original
        std::string filters;     // FFmpeg filter graph string
    };

    // Audio options (null = copy stream, empty object = disable)
    struct AudioOpts {
        std::string codec;       // e.g., "aac", "libmp3lame", "copy"
        int64_t bitrate = 0;     // 0 = keep original or default
        int sampleRate = 0;      // 0 = keep original
        int channels = 0;       // 0 = keep original
        std::string filters;     // FFmpeg filter graph string
    };

    VideoOpts video;
    AudioOpts audio;
    int threads = 0;             // 0 = auto
    std::string hwaccel;         // "cuda", "qsv", "vaapi", or empty (software)
};

// Transcode completion result
struct TranscodeResult {
    double duration;         // Total input duration (seconds)
    int64_t frames;         // Total video frames encoded
    int64_t audioFrames;    // Total audio samples encoded
    int64_t size;           // Final output file size (bytes)
    int64_t bitrate;        // Average output bitrate (bits/sec)
    double speed;           // Average processing speed
    int64_t timeMs;         // Total wall-clock time (ms)
    int dupFrames;          // Total duplicate frames
    int dropFrames;         // Total dropped frames
};

// Transcode error
struct TranscodeError {
    std::string message;    // Human-readable
    int code;               // FFmpeg error code
    std::string operation;  // Which phase: "open", "decode", "encode", "write"
    double timestamp;       // Where in the file (seconds)
    int64_t frame;         // Which frame
    int stream;             // Which stream
};

class FFmpegProcessor {
public:
    FFmpegProcessor();
    ~FFmpegProcessor();

    static std::string getVersion();

    // Probe - returns full metadata without opening decoders
    static ProbeResult probe(const char* path);

    // Get file metadata - lightweight static method
    static std::map<std::string, std::string> getFileMetadata(const char* path);

    // Thumbnail - seek to timestamp, decode single video frame, scale to width
    // Returns RGB24 data (caller must free with delete[])
    static ThumbnailResult thumbnail(const char* path, double timestamp, int targetWidth);

    // ==================== Waveform Generation ====================

    // Waveform - blocking, returns peaks for L/R channels
    WaveformResult getWaveform(int numPoints);

    // WaveformStreaming - with progress callback (return false to abort)
    WaveformResult getWaveformStreaming(int numPoints, int64_t chunkSizeBytes, WaveformProgressCallback callback);

    // ==================== Transcode to File ====================

    // Transcode - full encode pipeline with progress reporting
    // Runs synchronously on calling thread (call from AsyncWorker in binding)
    static bool transcode(const char* inputPath, const char* outputPath,
                   const TranscodeOptions& opts,
                   TranscodeProgressCallback progressCallback,
                   TranscodeResult& result,
                   TranscodeError& error);

    // Remux - stream copy without re-encode
    static bool remux(const char* inputPath, const char* outputPath,
               TranscodeProgressCallback progressCallback,
               TranscodeResult& result,
               TranscodeError& error);

    // Concat - join multiple files into one (stream copy)
    static bool concat(const char** inputPaths, int numInputs, const char* outputPath,
                       TranscodeProgressCallback progressCallback,
                       TranscodeResult& result,
                       TranscodeError& error);

    // ExtractStream - extract a single stream to a new file
    static bool extractStream(const char* inputPath, const char* outputPath, int streamIndex,
                                TranscodeProgressCallback progressCallback,
                                TranscodeResult& result,
                                TranscodeError& error);

    // ExtractAudio - decode audio from video, re-encode to target format
    static bool extractAudio(const char* inputPath, const char* outputPath,
                              const TranscodeOptions::AudioOpts& audioOpts,
                              TranscodeProgressCallback progressCallback,
                              TranscodeResult& result,
                              TranscodeError& error);

    // Get estimated duration from probe (used for progress reporting)
    static double probeDuration(const char* path);

    // Hardware acceleration helper
    static AVBufferRef* createHwDeviceContext(const char* hwaccel);

    // ==================== Audio Decode (Stateful) ====================

    // Open input and find audio stream for decoding
    bool openInput(const char* path, int targetSampleRate = 44100, int threads = 0);

    // Close input and free all resources
    void closeInput();

    // Seek to timestamp (seconds) - affects both audio and video position
    bool seek(double seconds);

    // Read decoded audio samples (interleaved float32 stereo)
    // Returns number of samples read (0 = EOF/error)
    int readAudio(float* outBuffer, int numSamples);

    // Get audio metadata
    double getDuration() const;
    int getSampleRate() const { return outputSampleRate; }
    int getChannels() const { return OUTPUT_CHANNELS; }
    int64_t getTotalSamples() const;
    bool isOpen() const { return formatCtx != nullptr; }

    // Audio stream index
    int getAudioStreamIndex() const { return audioStreamIndex; }

    // Get codec info
    std::string getCodecName() const;
    int getInputSampleRate() const { return audioCodecCtx ? audioCodecCtx->sample_rate : 0; }
    int getInputChannels() const { return audioCodecCtx ? audioCodecCtx->ch_layout.nb_channels : 0; }

    // ==================== Video Decode (Stateful) ====================

    // Open video stream for decoding (must be called after openInput if video not auto-opened)
    bool openVideoStream(int streamIndex = -1);

    // Read next video frame - writes directly into caller's buffer
    // Returns frame info (pts, width, height, etc.) or empty result on EOF/error
    struct VideoFrameResult {
        int width;
        int height;
        double pts;
        double duration;
        int frameNum;
        bool keyframe;
        std::string format;
    };
    VideoFrameResult readVideoFrame(uint8_t* outBuffer, int bufSize, int targetWidth = 0, int targetHeight = 0, const char* targetFormat = "rgb24");

    // Get current position (seconds) - works for video
    double getPosition() const;

    // Video stream info
    int getVideoStreamIndex() const { return videoStreamIndex; }
    int getVideoWidth() const { return videoCodecCtx ? videoCodecCtx->width : 0; }
    int getVideoHeight() const { return videoCodecCtx ? videoCodecCtx->height : 0; }
    std::string getVideoCodecName() const;
    double getVideoFPS() const;

    static const int OUTPUT_CHANNELS = 2;

private:
    static std::string getTagValue(AVDictionary* dict, const char* key);
    static double getStreamFrameRate(AVStream* stream);

    // Audio decode state
    AVFormatContext* formatCtx;
    AVCodecContext* audioCodecCtx;
    SwrContext* swrCtx;
    AVPacket* packet;
    AVFrame* frame;
    int audioStreamIndex;

    // Video decode state
    AVCodecContext* videoCodecCtx;
    AVFrame* videoFrame;
    int videoStreamIndex;
    int64_t videoFrameNum;

    // Output format
    int outputSampleRate;

    // Sample buffer (1 second of decoded audio)
    float* sampleBuffer;
    int sampleBufferSize;     // Total capacity in samples
    int samplesInBuffer;      // Current number of samples in buffer
    int bufferReadPos;        // Read position in samples

    // Three-phase EOF drain state
    bool eofSignaled;
    bool decoderDrained;
    bool resamplerDrained;

    bool initResampler();
    int decodeNextFrame();
    void flushBuffers();
};
