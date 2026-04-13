#include "processor.h"
#include <cstring>

FFmpegProcessor::FFmpegProcessor()
    : formatCtx(nullptr)
    , audioCodecCtx(nullptr)
    , swrCtx(nullptr)
    , packet(nullptr)
    , frame(nullptr)
    , audioStreamIndex(-1)
    , videoCodecCtx(nullptr)
    , videoFrame(nullptr)
    , videoStreamIndex(-1)
    , videoFrameNum(0)
    , outputSampleRate(44100)
    , sampleBuffer(nullptr)
    , sampleBufferSize(0)
    , samplesInBuffer(0)
    , bufferReadPos(0)
    , eofSignaled(false)
    , decoderDrained(false)
    , resamplerDrained(false)
{
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    videoFrame = av_frame_alloc();
}

FFmpegProcessor::~FFmpegProcessor() {
    closeInput();
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (videoFrame) av_frame_free(&videoFrame);
}

std::string FFmpegProcessor::getVersion() {
    return "0.1.0";
}

std::string FFmpegProcessor::getTagValue(AVDictionary* dict, const char* key) {
    if (!dict) return "";
    const AVDictionaryEntry* entry = av_dict_get(dict, key, nullptr, 0);
    return entry ? std::string(entry->value) : "";
}

double FFmpegProcessor::getStreamFrameRate(AVStream* stream) {
    if (!stream) return 0.0;

    // Try to get frame rate from stream
    AVRational fps = stream->avg_frame_rate;
    if (fps.den == 0 || fps.num == 0) {
        fps = stream->r_frame_rate;
    }
    if (fps.den == 0 || fps.num == 0) {
        return 0.0;
    }

    return static_cast<double>(fps.num) / fps.den;
}

// Static pixel format name lookup - avoids linking issues with av_get_pix_fmt_name
static const char* getPixelFormatName(int format) {
    switch (format) {
        case AV_PIX_FMT_YUV420P: return "yuv420p";
        case AV_PIX_FMT_YUV422P: return "yuv422p";
        case AV_PIX_FMT_YUV444P: return "yuv444p";
        case AV_PIX_FMT_YUV410P: return "yuv410p";
        case AV_PIX_FMT_YUV411P: return "yuv411p";
        case AV_PIX_FMT_YUVJ420P: return "yuvj420p";
        case AV_PIX_FMT_YUVJ422P: return "yuvj422p";
        case AV_PIX_FMT_YUVJ444P: return "yuvj444p";
        case AV_PIX_FMT_RGB24: return "rgb24";
        case AV_PIX_FMT_BGR24: return "bgr24";
        case AV_PIX_FMT_ARGB: return "argb";
        case AV_PIX_FMT_RGBA: return "rgba";
        case AV_PIX_FMT_ABGR: return "abgr";
        case AV_PIX_FMT_BGRA: return "bgra";
        case AV_PIX_FMT_GRAY8: return "gray8";
        case AV_PIX_FMT_GRAY16BE: return "gray16be";
        case AV_PIX_FMT_GRAY16LE: return "gray16le";
        case AV_PIX_FMT_NV12: return "nv12";
        case AV_PIX_FMT_NV21: return "nv21";
        case AV_PIX_FMT_UYVY422: return "uyvy422";
        case AV_PIX_FMT_YUYV422: return "yuyv422";
        case AV_PIX_FMT_YVYU422: return "yvyu422";
        default: return "unknown";
    }
}

ProbeResult FFmpegProcessor::probe(const char* path) {
    ProbeResult result;

    AVFormatContext* fmtCtx = nullptr;

    // Open input file
    if (avformat_open_input(&fmtCtx, path, nullptr, nullptr) < 0) {
        return result; // Return empty result on error
    }

    // Retrieve stream information
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return result;
    }

    // Format info
    result.formatName = fmtCtx->iformat ? fmtCtx->iformat->name : "";
    result.formatLong = (fmtCtx->iformat && fmtCtx->iformat->long_name)
        ? fmtCtx->iformat->long_name : "";
    result.duration = (fmtCtx->duration != AV_NOPTS_VALUE) ? fmtCtx->duration : 0;
    result.size = fmtCtx->pb ? avio_size(fmtCtx->pb) : 0;
    result.bitrate = fmtCtx->bit_rate > 0 ? static_cast<int>(fmtCtx->bit_rate) : 0;

    // Format tags
    const AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_iterate(fmtCtx->metadata, tag))) {
        result.tags[std::string(tag->key)] = std::string(tag->value);
    }

    // Stream info
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
        AVStream* stream = fmtCtx->streams[i];
        AVCodecParameters* codecParams = stream->codecpar;

        StreamInfo si;
        si.index = static_cast<int>(i);
        si.bitrate = codecParams->bit_rate > 0 ? static_cast<int>(codecParams->bit_rate) : 0;

        // Get codec info
        const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
        if (codec) {
            si.codec = codec->name;
            si.codecLong = codec->long_name ? codec->long_name : "";
        }

        // Stream type specific info
        switch (codecParams->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                si.type = "video";
                si.width = codecParams->width;
                si.height = codecParams->height;
                si.fps = getStreamFrameRate(stream);
                si.pixelFormat = getPixelFormatName(codecParams->format);
                break;

            case AVMEDIA_TYPE_AUDIO:
                si.type = "audio";
                si.sampleRate = codecParams->sample_rate;
                si.channels = codecParams->ch_layout.nb_channels;

                // Get channel layout name
                if (codecParams->ch_layout.nb_channels > 0 && codecParams->ch_layout.nb_channels <= 64) {
                    char layoutStr[64];
                    av_channel_layout_describe(&codecParams->ch_layout, layoutStr, sizeof(layoutStr));
                    si.channelLayout = layoutStr;
                }

                si.bitsPerSample = codecParams->bits_per_raw_sample > 0
                    ? codecParams->bits_per_raw_sample
                    : codecParams->bits_per_coded_sample;
                break;

            case AVMEDIA_TYPE_SUBTITLE:
                si.type = "subtitle";
                break;

            default:
                si.type = "data";
                break;
        }

        // Stream tags
        const AVDictionaryEntry* streamTag = nullptr;
        while ((streamTag = av_dict_iterate(stream->metadata, streamTag))) {
            si.tags[std::string(streamTag->key)] = std::string(streamTag->value);
        }

        result.streams.push_back(si);
    }

    avformat_close_input(&fmtCtx);
    return result;
}

// ==================== Audio Decode Implementation ====================

bool FFmpegProcessor::openInput(const char* path, int targetSampleRate, int threads) {
    outputSampleRate = (targetSampleRate > 0) ? targetSampleRate : 44100;

    // Open input file
    if (avformat_open_input(&formatCtx, path, nullptr, nullptr) < 0) {
        return false;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        avformat_close_input(&formatCtx);
        return false;
    }

    // Find audio stream
    audioStreamIndex = -1;
    for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = static_cast<int>(i);
            break;
        }
    }

    if (audioStreamIndex == -1) {
        avformat_close_input(&formatCtx);
        return false;
    }

    // Get codec parameters
    AVCodecParameters* codecParams = formatCtx->streams[audioStreamIndex]->codecpar;

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        avformat_close_input(&formatCtx);
        return false;
    }

    // Allocate codec context
    audioCodecCtx = avcodec_alloc_context3(codec);
    if (!audioCodecCtx) {
        avformat_close_input(&formatCtx);
        return false;
    }

    // Copy codec parameters to context
    if (avcodec_parameters_to_context(audioCodecCtx, codecParams) < 0) {
        avcodec_free_context(&audioCodecCtx);
        avformat_close_input(&formatCtx);
        return false;
    }

    // Configure threading
    if (threads > 0) {
        audioCodecCtx->thread_count = threads;
    }
    audioCodecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    // Open codec
    if (avcodec_open2(audioCodecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&audioCodecCtx);
        avformat_close_input(&formatCtx);
        return false;
    }

    // Initialize resampler
    if (!initResampler()) {
        closeInput();
        return false;
    }

    // Allocate sample buffer (1 second of audio at output sample rate, stereo)
    sampleBufferSize = outputSampleRate * OUTPUT_CHANNELS;
    sampleBuffer = new float[sampleBufferSize];

    eofSignaled = false;
    decoderDrained = false;
    resamplerDrained = false;
    samplesInBuffer = 0;
    bufferReadPos = 0;

    return true;
}

bool FFmpegProcessor::initResampler() {
    // Determine input channel layout (FFmpeg 7.0+ uses ch_layout)
    AVChannelLayout in_ch_layout;
    if (audioCodecCtx->ch_layout.nb_channels > 0) {
        in_ch_layout = audioCodecCtx->ch_layout;
    } else {
        av_channel_layout_default(&in_ch_layout, 2);
    }

    // Output channel layout (stereo)
    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;

    // Create resampler context (FFmpeg 7.0+ API)
    int ret = swr_alloc_set_opts2(
        &swrCtx,
        &out_ch_layout,            // Output channel layout
        AV_SAMPLE_FMT_FLT,         // Output sample format (float32)
        outputSampleRate,          // Output sample rate
        &in_ch_layout,              // Input channel layout
        audioCodecCtx->sample_fmt,       // Input sample format
        audioCodecCtx->sample_rate,      // Input sample rate
        0, nullptr
    );

    if (ret < 0 || !swrCtx) {
        return false;
    }

    if (swr_init(swrCtx) < 0) {
        swr_free(&swrCtx);
        return false;
    }

    return true;
}

void FFmpegProcessor::closeInput() {
    delete[] sampleBuffer;
    sampleBuffer = nullptr;

    if (swrCtx) {
        swr_free(&swrCtx);
        swrCtx = nullptr;
    }

    if (audioCodecCtx) {
        avcodec_free_context(&audioCodecCtx);
        audioCodecCtx = nullptr;
    }

    if (videoCodecCtx) {
        avcodec_free_context(&videoCodecCtx);
        videoCodecCtx = nullptr;
    }

    if (formatCtx) {
        avformat_close_input(&formatCtx);
        formatCtx = nullptr;
    }

    audioStreamIndex = -1;
    videoStreamIndex = -1;
    samplesInBuffer = 0;
    bufferReadPos = 0;
    videoFrameNum = 0;
    eofSignaled = false;
    decoderDrained = false;
    resamplerDrained = false;
}

void FFmpegProcessor::flushBuffers() {
    samplesInBuffer = 0;
    bufferReadPos = 0;
}

bool FFmpegProcessor::seek(double seconds) {
    if (!formatCtx) return false;

    // Seek on audio stream if open
    if (audioStreamIndex >= 0) {
        AVStream* stream = formatCtx->streams[audioStreamIndex];
        int64_t timestamp = static_cast<int64_t>(seconds * AV_TIME_BASE);

        av_seek_frame(formatCtx, audioStreamIndex,
                     av_rescale_q(timestamp, AV_TIME_BASE_Q, stream->time_base),
                     AVSEEK_FLAG_BACKWARD);

        // Flush audio codec buffers
        avcodec_flush_buffers(audioCodecCtx);

        // Reset resampler state (swr_close + swr_init pattern)
        if (swrCtx) {
            swr_close(swrCtx);
            swr_init(swrCtx);
        }

        // Clear audio sample buffer
        flushBuffers();

        // Reset audio EOF/drain state
        eofSignaled = false;
        decoderDrained = false;
        resamplerDrained = false;
    }

    // Seek on video stream if open
    if (videoStreamIndex >= 0) {
        AVStream* stream = formatCtx->streams[videoStreamIndex];
        int64_t timestamp = static_cast<int64_t>(seconds * AV_TIME_BASE);

        av_seek_frame(formatCtx, videoStreamIndex,
                     av_rescale_q(timestamp, AV_TIME_BASE_Q, stream->time_base),
                     AVSEEK_FLAG_BACKWARD);

        // Flush video codec buffers
        avcodec_flush_buffers(videoCodecCtx);

        // Reset video frame counter
        videoFrameNum = 0;
    }

    return true;
}

int FFmpegProcessor::decodeNextFrame() {
    while (true) {
        // Try to get decoded frame from codec
        int ret = avcodec_receive_frame(audioCodecCtx, frame);
        if (ret == 0) {
            // Resample directly
            uint8_t* output_buffer = reinterpret_cast<uint8_t*>(sampleBuffer);
            int out_samples = swr_convert(
                swrCtx,
                &output_buffer,
                sampleBufferSize / OUTPUT_CHANNELS,
                const_cast<const uint8_t**>(frame->data),
                frame->nb_samples
            );
            av_frame_unref(frame);

            if (out_samples < 0) return -1;

            samplesInBuffer = out_samples * OUTPUT_CHANNELS;
            bufferReadPos = 0;
            return samplesInBuffer;
        }

        if (ret == AVERROR_EOF) {
            decoderDrained = true;
        } else if (ret != AVERROR(EAGAIN)) {
            return -1;
        }

        // Phase 2: If decoder drained, try draining resampler (it can hold delayed samples)
        if (decoderDrained && !resamplerDrained) {
            uint8_t* output_buffer = reinterpret_cast<uint8_t*>(sampleBuffer);
            int out_samples = swr_convert(
                swrCtx,
                &output_buffer,
                sampleBufferSize / OUTPUT_CHANNELS,
                nullptr,
                0
            );
            if (out_samples < 0) return -1;
            if (out_samples > 0) {
                samplesInBuffer = out_samples * OUTPUT_CHANNELS;
                bufferReadPos = 0;
                return samplesInBuffer;
            }
            resamplerDrained = true;
            return 0;
        }

        // Phase 3: Need more input packets
        if (!eofSignaled) {
            ret = av_read_frame(formatCtx, packet);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    eofSignaled = true;
                    avcodec_send_packet(audioCodecCtx, nullptr);
                    continue;
                }
                return -1;
            }

            if (packet->stream_index != audioStreamIndex) {
                av_packet_unref(packet);
                continue;
            }

            ret = avcodec_send_packet(audioCodecCtx, packet);
            av_packet_unref(packet);
            if (ret < 0) return -1;
            continue;
        }

        // EOF already signaled and not drained yet - keep trying
        if (!decoderDrained) {
            continue;
        }

        return 0;
    }
}

int FFmpegProcessor::readAudio(float* outBuffer, int numSamples) {
    if (!formatCtx || !outBuffer) return 0;

    int totalRead = 0;

    while (totalRead < numSamples) {
        // If buffer is empty, decode next frame
        if (bufferReadPos >= samplesInBuffer) {
            int decoded = decodeNextFrame();
            if (decoded <= 0) {
                break; // End of file or error
            }
        }

        // Copy from internal buffer
        int available = samplesInBuffer - bufferReadPos;
        int toCopy = std::min(available, numSamples - totalRead);

        memcpy(outBuffer + totalRead, sampleBuffer + bufferReadPos, toCopy * sizeof(float));

        bufferReadPos += toCopy;
        totalRead += toCopy;
    }

    return totalRead;
}

double FFmpegProcessor::getDuration() const {
    if (!formatCtx) return 0.0;

    if (formatCtx->duration != AV_NOPTS_VALUE) {
        return static_cast<double>(formatCtx->duration) / AV_TIME_BASE;
    }

    AVStream* stream = formatCtx->streams[audioStreamIndex];
    if (stream->duration != AV_NOPTS_VALUE) {
        return static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    }

    return 0.0;
}

int64_t FFmpegProcessor::getTotalSamples() const {
    return static_cast<int64_t>(getDuration() * outputSampleRate);
}

// ==================== Waveform Generation ====================

WaveformResult FFmpegProcessor::getWaveform(int numPoints) {
    WaveformResult data;
    data.points = numPoints;
    data.peaksL.resize(numPoints, 0.0f);
    data.peaksR.resize(numPoints, 0.0f);

    if (!formatCtx || audioStreamIndex < 0) return data;

    // Seek to beginning
    seek(0);

    const int64_t totalFrames = getTotalSamples();
    const int64_t framesPerPoint = std::max((int64_t)1, totalFrames / numPoints);

    int currentPoint = 0;
    int64_t pointFramesAccumulated = 0;
    float maxL = 0.0f;
    float maxR = 0.0f;

    const int readBatchSize = 4096;
    float batchBuffer[readBatchSize * OUTPUT_CHANNELS];

    while (currentPoint < numPoints) {
        int readSamples = readAudio(batchBuffer, readBatchSize);
        if (readSamples <= 0) break;

        for (int i = 0; i < readSamples; i += OUTPUT_CHANNELS) {
            float l = std::abs(batchBuffer[i]);
            float r = std::abs(batchBuffer[i + 1]);

            if (l > maxL) maxL = l;
            if (r > maxR) maxR = r;

            pointFramesAccumulated++;

            if (pointFramesAccumulated >= framesPerPoint) {
                if (currentPoint < numPoints) {
                    data.peaksL[currentPoint] = maxL;
                    data.peaksR[currentPoint] = maxR;
                    currentPoint++;
                }
                maxL = 0.0f;
                maxR = 0.0f;
                pointFramesAccumulated = 0;

                if (currentPoint >= numPoints) break;
            }
        }
    }

    // Handle last point if file ended early or rounding
    if (currentPoint < numPoints && pointFramesAccumulated > 0) {
        data.peaksL[currentPoint] = maxL;
        data.peaksR[currentPoint] = maxR;
    }

    data.duration = getDuration();
    return data;
}

WaveformResult FFmpegProcessor::getWaveformStreaming(
    int numPoints, int64_t chunkSizeBytes, WaveformProgressCallback callback) {

    WaveformResult data;
    data.points = numPoints;
    data.peaksL.resize(numPoints, 0.0f);
    data.peaksR.resize(numPoints, 0.0f);

    if (!formatCtx || audioStreamIndex < 0) return data;

    // Seek to beginning
    seek(0);

    const int64_t totalFrames = getTotalSamples();
    const int64_t framesPerPoint = std::max((int64_t)1, totalFrames / numPoints);

    int currentPoint = 0;
    int64_t pointFramesAccumulated = 0;
    int64_t bytesProcessed = 0;
    int64_t nextCallbackAt = chunkSizeBytes;
    float maxL = 0.0f;
    float maxR = 0.0f;

    const int readBatchSize = 4096;
    float batchBuffer[readBatchSize * OUTPUT_CHANNELS];
    const int bytesPerSample = sizeof(float) * OUTPUT_CHANNELS;

    while (currentPoint < numPoints) {
        int readSamples = readAudio(batchBuffer, readBatchSize);
        if (readSamples <= 0) break;

        bytesProcessed += readSamples * bytesPerSample;

        for (int i = 0; i < readSamples; i += OUTPUT_CHANNELS) {
            float l = std::abs(batchBuffer[i]);
            float r = std::abs(batchBuffer[i + 1]);

            if (l > maxL) maxL = l;
            if (r > maxR) maxR = r;

            pointFramesAccumulated++;

            if (pointFramesAccumulated >= framesPerPoint) {
                if (currentPoint < numPoints) {
                    data.peaksL[currentPoint] = maxL;
                    data.peaksR[currentPoint] = maxR;
                    currentPoint++;
                }
                maxL = 0.0f;
                maxR = 0.0f;
                pointFramesAccumulated = 0;

                if (currentPoint >= numPoints) break;
            }
        }

        // Call callback every chunk
        if (callback && bytesProcessed >= nextCallbackAt) {
            float progress = (float)currentPoint / (float)numPoints;
            bool shouldContinue = callback(data, progress);
            if (!shouldContinue) {
                return data;
            }
            nextCallbackAt += chunkSizeBytes;
        }
    }

    // Handle last point
    if (currentPoint < numPoints && pointFramesAccumulated > 0) {
        data.peaksL[currentPoint] = maxL;
        data.peaksR[currentPoint] = maxR;
    }

    // Final callback
    if (callback) {
        callback(data, 1.0f);
    }

    data.duration = getDuration();
    return data;
}

std::string FFmpegProcessor::getCodecName() const {
    if (!audioCodecCtx) return "";
    const AVCodec* codec = avcodec_find_decoder(audioCodecCtx->codec_id);
    return codec ? codec->name : "";
}

std::string FFmpegProcessor::getVideoCodecName() const {
    if (!videoCodecCtx) return "";
    const AVCodec* codec = avcodec_find_decoder(videoCodecCtx->codec_id);
    return codec ? codec->name : "";
}

double FFmpegProcessor::getVideoFPS() const {
    if (!formatCtx || videoStreamIndex < 0) return 0.0;
    AVStream* stream = formatCtx->streams[videoStreamIndex];
    return getStreamFrameRate(stream);
}

double FFmpegProcessor::getPosition() const {
    if (!formatCtx) return 0.0;
    if (videoStreamIndex >= 0 && videoFrame && videoFrame->pts != AV_NOPTS_VALUE) {
        AVStream* stream = formatCtx->streams[videoStreamIndex];
        return videoFrame->pts * av_q2d(stream->time_base);
    }
    return 0.0;
}

// ==================== Video Decode Implementation ====================

bool FFmpegProcessor::openVideoStream(int streamIndex) {
    if (!formatCtx) return false;

    // If streamIndex not specified, find video stream
    if (streamIndex < 0) {
        for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
            if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStreamIndex = static_cast<int>(i);
                break;
            }
        }
        if (videoStreamIndex < 0) return false;
    } else {
        videoStreamIndex = streamIndex;
    }

    AVStream* stream = formatCtx->streams[videoStreamIndex];
    AVCodecParameters* codecParams = stream->codecpar;

    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) return false;

    videoCodecCtx = avcodec_alloc_context3(codec);
    if (!videoCodecCtx) return false;

    if (avcodec_parameters_to_context(videoCodecCtx, codecParams) < 0) {
        avcodec_free_context(&videoCodecCtx);
        return false;
    }

    if (avcodec_open2(videoCodecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&videoCodecCtx);
        return false;
    }

    // Seek to the beginning of the video stream
    av_seek_frame(formatCtx, videoStreamIndex, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(videoCodecCtx);

    videoFrameNum = 0;
    return true;
}

FFmpegProcessor::VideoFrameResult FFmpegProcessor::readVideoFrame(
    uint8_t* outBuffer, int bufSize,
    int targetWidth, int targetHeight,
    const char* targetFormat) {

    VideoFrameResult result = {0, 0, 0.0, 0.0, 0, false, "unknown"};

    if (!formatCtx || videoStreamIndex < 0 || !videoCodecCtx) {
        return result;
    }

    // Calculate expected buffer size for YUV444P (worst case)
    int srcWidth = videoCodecCtx->width;
    int srcHeight = videoCodecCtx->height;
    int dstWidth = targetWidth > 0 ? targetWidth : srcWidth;
    int dstHeight = targetHeight > 0 ? targetHeight : srcHeight;
    dstWidth = (dstWidth + 1) & ~1;
    dstHeight = (dstHeight + 1) & ~1;

    // Determine target format
    AVPixelFormat targetPixFmt = AV_PIX_FMT_RGB24;
    bool doScale = false;
    if (targetFormat) {
        if (strcmp(targetFormat, "yuv420p") == 0) targetPixFmt = AV_PIX_FMT_YUV420P;
        else if (strcmp(targetFormat, "rgb24") == 0) targetPixFmt = AV_PIX_FMT_RGB24;
        else if (strcmp(targetFormat, "rgba") == 0) targetPixFmt = AV_PIX_FMT_RGBA;
        else if (strcmp(targetFormat, "bgr24") == 0) targetPixFmt = AV_PIX_FMT_BGR24;
    }

    // For scaling, we need swscale
    SwsContext* swsCtx = nullptr;
    AVFrame* scaledFrame = nullptr;

    doScale = (dstWidth != srcWidth) || (dstHeight != srcHeight) ||
              (videoCodecCtx->pix_fmt != targetPixFmt);

    if (doScale) {
        swsCtx = sws_getContext(
            srcWidth, srcHeight, videoCodecCtx->pix_fmt,
            dstWidth, dstHeight, targetPixFmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!swsCtx) {
            return result;
        }

        scaledFrame = av_frame_alloc();
        if (!scaledFrame) {
            sws_freeContext(swsCtx);
            return result;
        }
        scaledFrame->format = targetPixFmt;
        scaledFrame->width = dstWidth;
        scaledFrame->height = dstHeight;
        av_frame_get_buffer(scaledFrame, 0);
    }

    // Decode frames until we get one
    while (av_read_frame(formatCtx, packet) >= 0) {
        if (packet->stream_index == videoStreamIndex) {
            int ret = avcodec_send_packet(videoCodecCtx, packet);
            if (ret < 0) {
                av_packet_unref(packet);
                continue;
            }

            while (avcodec_receive_frame(videoCodecCtx, videoFrame) >= 0) {
                if (videoFrame->width <= 0 || videoFrame->height <= 0) {
                    av_frame_unref(videoFrame);
                    continue;
                }

                AVStream* stream = formatCtx->streams[videoStreamIndex];

                if (doScale && swsCtx) {
                    int h = sws_scale(swsCtx, videoFrame->data, videoFrame->linesize,
                                     0, videoFrame->height,
                                     scaledFrame->data, scaledFrame->linesize);
                    if (h > 0) {
                        // Copy scaled data - use actual linesize from scaled frame
                        int copyLineSize = scaledFrame->linesize[0];
                        for (int y = 0; y < dstHeight; y++) {
                            memcpy(outBuffer + y * copyLineSize,
                                   scaledFrame->data[0] + y * scaledFrame->linesize[0],
                                   copyLineSize);
                        }
                        result.width = dstWidth;
                        result.height = dstHeight;
                        result.format = getPixelFormatName(targetPixFmt);
                    }
                } else {
                    // Direct copy
                    int lineSize = videoFrame->linesize[0];
                    for (int y = 0; y < videoFrame->height; y++) {
                        memcpy(outBuffer + y * lineSize,
                               videoFrame->data[0] + y * lineSize,
                               lineSize);
                    }
                    result.width = videoFrame->width;
                    result.height = videoFrame->height;
                    result.format = getPixelFormatName((AVPixelFormat)videoFrame->format);
                }

                result.pts = videoFrame->pts != AV_NOPTS_VALUE ?
                             videoFrame->pts * av_q2d(stream->time_base) : 0.0;
                result.frameNum = videoFrameNum++;
                result.keyframe = (videoFrame->flags & AV_FRAME_FLAG_KEY) != 0;

                av_packet_unref(packet);
                if (scaledFrame) av_frame_free(&scaledFrame);
                if (swsCtx) sws_freeContext(swsCtx);
                av_frame_unref(videoFrame);
                return result;
            }
        }
        av_packet_unref(packet);
    }

    // Try to drain decoder
    avcodec_send_packet(videoCodecCtx, nullptr);
    while (avcodec_receive_frame(videoCodecCtx, videoFrame) >= 0) {
        if (videoFrame->width <= 0 || videoFrame->height <= 0) {
            av_frame_unref(videoFrame);
            continue;
        }

        AVStream* stream = formatCtx->streams[videoStreamIndex];

        if (doScale && swsCtx) {
            int h = sws_scale(swsCtx, videoFrame->data, videoFrame->linesize,
                             0, videoFrame->height,
                             scaledFrame->data, scaledFrame->linesize);
            if (h > 0) {
                // Copy scaled data - use actual linesize from scaled frame
                int copyLineSize = scaledFrame->linesize[0];
                for (int y = 0; y < dstHeight; y++) {
                    memcpy(outBuffer + y * copyLineSize,
                           scaledFrame->data[0] + y * scaledFrame->linesize[0],
                           copyLineSize);
                }
                result.width = dstWidth;
                result.height = dstHeight;
                result.format = getPixelFormatName(targetPixFmt);
            }
        } else {
            int lineSize = videoFrame->linesize[0];
            for (int y = 0; y < videoFrame->height; y++) {
                memcpy(outBuffer + y * lineSize,
                       videoFrame->data[0] + y * lineSize,
                       lineSize);
            }
            result.width = videoFrame->width;
            result.height = videoFrame->height;
            result.format = getPixelFormatName((AVPixelFormat)videoFrame->format);
        }

        result.pts = videoFrame->pts != AV_NOPTS_VALUE ?
                     videoFrame->pts * av_q2d(stream->time_base) : 0.0;
        result.frameNum = videoFrameNum++;
        result.keyframe = (videoFrame->flags & AV_FRAME_FLAG_KEY) != 0;

        if (scaledFrame) av_frame_free(&scaledFrame);
        if (swsCtx) sws_freeContext(swsCtx);
        av_frame_unref(videoFrame);
        return result;
    }

    if (scaledFrame) av_frame_free(&scaledFrame);
    if (swsCtx) sws_freeContext(swsCtx);

    return result;
}

std::map<std::string, std::string> FFmpegProcessor::getFileMetadata(const char* path) {
    std::map<std::string, std::string> metadata;

    AVFormatContext* fmtCtx = nullptr;

    if (avformat_open_input(&fmtCtx, path, nullptr, nullptr) < 0) {
        return metadata;
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return metadata;
    }

    // Collect format-level tags
    const AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_iterate(fmtCtx->metadata, tag))) {
        metadata[std::string(tag->key)] = std::string(tag->value);
    }

    avformat_close_input(&fmtCtx);
    return metadata;
}

ThumbnailResult FFmpegProcessor::thumbnail(const char* path, double timestamp, int targetWidth) {
    ThumbnailResult result;

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, path, nullptr, nullptr) < 0) {
        return result;
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return result;
    }

    // Find video stream
    int videoStreamIdx = -1;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIdx = static_cast<int>(i);
            break;
        }
    }

    if (videoStreamIdx < 0) {
        avformat_close_input(&fmtCtx);
        return result;
    }

    AVStream* videoStream = fmtCtx->streams[videoStreamIdx];
    AVCodecParameters* codecParams = videoStream->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);

    if (!codec) {
        avformat_close_input(&fmtCtx);
        return result;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        avformat_close_input(&fmtCtx);
        return result;
    }

    avcodec_parameters_to_context(codecCtx, codecParams);

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }

    // Seek to timestamp (in stream's time base)
    int64_t seekTimestamp = av_rescale_q(static_cast<int64_t>(timestamp * 1000000),
        av_make_q(1, 1000000), videoStream->time_base);

    // Use avformat_seek_file with wide bounds for better keyframe discovery
    int seekRet = avformat_seek_file(fmtCtx, videoStreamIdx, seekTimestamp - 1000000, seekTimestamp, seekTimestamp + 1000000, AVSEEK_FLAG_BACKWARD);
    if (seekRet < 0) {
        // Fallback to av_seek_frame
        seekRet = av_seek_frame(fmtCtx, videoStreamIdx, seekTimestamp, AVSEEK_FLAG_BACKWARD);
    }
    if (seekRet < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }

    // Flush codec buffers after seek
    avcodec_flush_buffers(codecCtx);

    // Decode frames until we get one at or after the requested timestamp
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();

    if (!packet || !frame || !rgbFrame) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&rgbFrame);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }

    // Allocate RGB buffer
    int64_t srcWidth = codecParams->width;
    int64_t srcHeight = codecParams->height;
    int dstWidth = targetWidth;
    int dstHeight = static_cast<int>((srcHeight * targetWidth) / srcWidth);

    // Ensure dimensions are even
    dstWidth = (dstWidth + 1) & ~1;
    dstHeight = (dstHeight + 1) & ~1;

    int rgbBufferSize = dstWidth * dstHeight * 3;

    // Setup RGB frame for sws_scale
    rgbFrame->format = AV_PIX_FMT_RGB24;
    rgbFrame->width = dstWidth;
    rgbFrame->height = dstHeight;
    av_frame_get_buffer(rgbFrame, 0);

    // Setup SwsContext for scaling
    SwsContext* swsCtx = sws_getContext(
        static_cast<int>(srcWidth), static_cast<int>(srcHeight), codecCtx->pix_fmt,
        dstWidth, dstHeight, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!swsCtx) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&rgbFrame);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return result;
    }

    bool frameFound = false;
    double foundPts = 0.0;
    bool isKeyframe = false;

    // Read and decode packets
    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index == videoStreamIdx) {
            int sendRet = avcodec_send_packet(codecCtx, packet);
            if (sendRet < 0) {
                av_packet_unref(packet);
                continue;
            }

            while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                double pts = frame->pts * av_q2d(videoStream->time_base);

                if (pts >= timestamp - 0.001) {
                    // Scale this frame to RGB
                    sws_scale(swsCtx, frame->data, frame->linesize, 0, frame->height,
                               rgbFrame->data, rgbFrame->linesize);

                    foundPts = pts;
                    isKeyframe = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
                    frameFound = true;
                    break;
                }
            }
        }
        av_packet_unref(packet);

        if (frameFound) break;
    }

    if (frameFound) {
        // Copy RGB data to result
        result.data = new uint8_t[rgbBufferSize];
        memcpy(result.data, rgbFrame->data[0], rgbBufferSize);
        result.width = dstWidth;
        result.height = dstHeight;
        result.pts = foundPts;
        result.keyframe = isKeyframe;
    }

    sws_freeContext(swsCtx);
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&rgbFrame);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    return result;
}

// ==================== Transcode to File ====================

// Need time.h for av_gettime()
extern "C" {
#include <libavutil/time.h>
}

// Helper: Create hardware device context for given hwaccel type
AVBufferRef* FFmpegProcessor::createHwDeviceContext(const char* hwaccel) {
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    
    if (strcmp(hwaccel, "cuda") == 0) {
        type = AV_HWDEVICE_TYPE_CUDA;
    } else if (strcmp(hwaccel, "qsv") == 0) {
        type = AV_HWDEVICE_TYPE_QSV;
    } else if (strcmp(hwaccel, "vaapi") == 0) {
        type = AV_HWDEVICE_TYPE_VAAPI;
    } else if (strcmp(hwaccel, "d3d11va") == 0) {
        type = AV_HWDEVICE_TYPE_D3D11VA;
    } else {
        return nullptr;
    }
    
    AVBufferRef* hwDeviceCtx = nullptr;
    int ret = av_hwdevice_ctx_create(&hwDeviceCtx, type, nullptr, nullptr, 0);
    if (ret < 0) {
        return nullptr;
    }
    
    return hwDeviceCtx;
}

double FFmpegProcessor::probeDuration(const char* path) {
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, path, nullptr, nullptr) < 0) {
        return 0.0;
    }
    double duration = 0.0;
    if (fmtCtx->duration != AV_NOPTS_VALUE) {
        duration = static_cast<double>(fmtCtx->duration) / AV_TIME_BASE;
    }
    avformat_close_input(&fmtCtx);
    return duration;
}

static const char* av_err_to_string(int err) {
    static char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

// Helper to find first supported sample format for an encoder
static AVSampleFormat find_best_sample_fmt(const AVCodec* codec, AVSampleFormat preferred) {
    if (!codec->sample_fmts) return preferred;
    const AVSampleFormat* p = codec->sample_fmts;
    // Prefer FLTP if available (float planar)
    while (*p != AV_SAMPLE_FMT_NONE) {
        if (*p == AV_SAMPLE_FMT_FLTP) return AV_SAMPLE_FMT_FLTP;
        p++;
    }
    // Fall back to first supported format
    return codec->sample_fmts[0];
}

bool FFmpegProcessor::transcode(const char* inputPath, const char* outputPath,
                                 const TranscodeOptions& opts,
                                 TranscodeProgressCallback progressCallback,
                                 TranscodeResult& result,
                                 TranscodeError& error) {
    // Initialize result/error
    result = TranscodeResult();
    error = TranscodeError();
    error.code = 0;

    AVFormatContext* ifmtCtx = nullptr;
    AVFormatContext* ofmtCtx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* scaledFrame = nullptr;

    int ret = 0;
    bool success = false;

    // Output stream pointers (declared at function scope for goto cleanup access)
    AVStream* videoOutStream = nullptr;
    AVStream* audioOutStream = nullptr;

    // Open input
    if ((ret = avformat_open_input(&ifmtCtx, inputPath, nullptr, nullptr)) < 0) {
        error.message = std::string("Failed to open input: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    if ((ret = avformat_find_stream_info(ifmtCtx, nullptr)) < 0) {
        error.message = std::string("Failed to find stream info: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    // Find video and audio streams
    int videoStreamIdx = -1;
    int audioStreamIdx = -1;
    for (unsigned int i = 0; i < ifmtCtx->nb_streams; i++) {
        if (ifmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStreamIdx == -1) {
            videoStreamIdx = static_cast<int>(i);
        }
        if (ifmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStreamIdx == -1) {
            audioStreamIdx = static_cast<int>(i);
        }
    }

    // Create output context
    if ((ret = avformat_alloc_output_context2(&ofmtCtx, nullptr, nullptr, outputPath)) < 0) {
        error.message = std::string("Failed to create output context: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    // Video encoding pipeline
    AVCodecContext* videoDecCtx = nullptr;
    AVCodecContext* videoEncCtx = nullptr;
    SwsContext* swsCtx = nullptr;
    AVFilterGraph* videoFilterGraph = nullptr;
    AVFilterContext* videoBuffersrcCtx = nullptr;
    AVFilterContext* videoBuffersinkCtx = nullptr;
    bool videoEnabled = (videoStreamIdx >= 0 && !opts.video.codec.empty() && opts.video.codec != "copy");
    bool videoCopy = (videoStreamIdx >= 0 && opts.video.codec == "copy");

    if (videoEnabled) {
        AVStream* videoStream = ifmtCtx->streams[videoStreamIdx];
        const AVCodec* videoDec = avcodec_find_decoder(videoStream->codecpar->codec_id);
        if (!videoDec) {
            error.message = "Video decoder not found";
            error.operation = "open";
            goto cleanup;
        }

        videoDecCtx = avcodec_alloc_context3(videoDec);
        if (!videoDecCtx) {
            error.message = "Failed to allocate video decoder context";
            error.operation = "open";
            goto cleanup;
        }
        avcodec_parameters_to_context(videoDecCtx, videoStream->codecpar);
        if (opts.threads > 0) videoDecCtx->thread_count = opts.threads;
        videoDecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

        // Hardware decode setup
        AVBufferRef* hwDeviceCtx = nullptr;
        bool hwDecode = !opts.hwaccel.empty();
        if (hwDecode) {
            hwDeviceCtx = createHwDeviceContext(opts.hwaccel.c_str());
            if (hwDeviceCtx) {
                videoDecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
                // Get supported HW pixel formats
                const enum AVPixelFormat* hwPixFmt = videoDec->pix_fmts;
                if (hwPixFmt && *hwPixFmt != AV_PIX_FMT_NONE) {
                    videoDecCtx->pix_fmt = *hwPixFmt;
                }
            }
        }

        if ((ret = avcodec_open2(videoDecCtx, videoDec, nullptr)) < 0) {
            error.message = std::string("Failed to open video decoder: ") + av_err_to_string(ret);
            error.operation = "open";
            goto cleanup;
        }

        const AVCodec* videoEnc = avcodec_find_encoder_by_name(opts.video.codec.c_str());
        if (!videoEnc) {
            error.message = std::string("Video encoder not found: ") + opts.video.codec;
            error.operation = "open";
            goto cleanup;
        }

        videoEncCtx = avcodec_alloc_context3(videoEnc);
        if (!videoEncCtx) {
            error.message = "Failed to allocate video encoder context";
            error.operation = "open";
            goto cleanup;
        }

        int encWidth = opts.video.width > 0 ? opts.video.width : videoStream->codecpar->width;
        int encHeight = opts.video.height > 0 ? opts.video.height : videoStream->codecpar->height;
        // Ensure dimensions are even (required by many codecs)
        encWidth = (encWidth + 1) & ~1;
        encHeight = (encHeight + 1) & ~1;

        videoEncCtx->width = encWidth;
        videoEncCtx->height = encHeight;
        videoEncCtx->time_base = videoStream->time_base;

        // Pixel format - check if hardware encoder
        AVPixelFormat pixFmt = AV_PIX_FMT_YUV420P;
        bool hwEncode = !opts.hwaccel.empty();
        AVBufferRef* hwFramesCtx = nullptr;

        if (hwEncode && hwDeviceCtx) {
            // Get supported pixel formats from encoder
            const enum AVPixelFormat* encPixFmts = videoEnc->pix_fmts;
            AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;
            
            // Find HW pixel format for this encoder
            if (opts.hwaccel == "cuda") {
                hwPixFmt = AV_PIX_FMT_CUDA;
            } else if (opts.hwaccel == "qsv") {
                hwPixFmt = AV_PIX_FMT_QSV;
            } else if (opts.hwaccel == "vaapi") {
                hwPixFmt = AV_PIX_FMT_VAAPI;
            } else if (opts.hwaccel == "d3d11va") {
                hwPixFmt = AV_PIX_FMT_D3D11;
            }
            
            // Check if encoder supports the HW format
            if (hwPixFmt != AV_PIX_FMT_NONE) {
                bool supported = false;
                if (encPixFmts) {
                    for (int i = 0; encPixFmts[i] != AV_PIX_FMT_NONE; i++) {
                        if (encPixFmts[i] == hwPixFmt) {
                            supported = true;
                            break;
                        }
                    }
                }
                
                if (supported) {
                    // Create HW frames context for encoder
                    hwFramesCtx = av_hwframe_ctx_alloc(hwDeviceCtx);
                    if (hwFramesCtx) {
                        AVHWFramesContext* framesCtx = (AVHWFramesContext*)hwFramesCtx->data;
                        framesCtx->format = hwPixFmt;
                        framesCtx->sw_format = AV_PIX_FMT_NV12; // Common intermediate format
                        framesCtx->width = encWidth;
                        framesCtx->height = encHeight;
                        
                        ret = av_hwframe_ctx_init(hwFramesCtx);
                        if (ret >= 0) {
                            videoEncCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
                            pixFmt = hwPixFmt;
                        }
                    }
                }
            }
        } else if (!opts.video.pixelFormat.empty()) {
            if (opts.video.pixelFormat == "yuv420p") pixFmt = AV_PIX_FMT_YUV420P;
            else if (opts.video.pixelFormat == "yuv422p") pixFmt = AV_PIX_FMT_YUV422P;
            else if (opts.video.pixelFormat == "yuv444p") pixFmt = AV_PIX_FMT_YUV444P;
            else if (opts.video.pixelFormat == "rgb24") pixFmt = AV_PIX_FMT_RGB24;
            else if (opts.video.pixelFormat == "bgr24") pixFmt = AV_PIX_FMT_BGR24;
            else if (opts.video.pixelFormat == "yuv420p10") pixFmt = AV_PIX_FMT_YUV420P10LE;
        }
        videoEncCtx->pix_fmt = pixFmt;

        if (opts.video.bitrate > 0) {
            videoEncCtx->bit_rate = opts.video.bitrate;
        }

        if (opts.video.fps > 0) {
            videoEncCtx->framerate = av_make_q(opts.video.fps, 1);
        }

        // If filter graph with scale is used, try to parse output dimensions from filter string
        if (!opts.video.filters.empty()) {
            // Parse scale=W:H from filter string
            const char* filterStr = opts.video.filters.c_str();
            const char* scalePos = strstr(filterStr, "scale=");
            if (scalePos) {
                int w = 0, h = 0;
                if (sscanf(scalePos + 6, "%d:%d", &w, &h) == 2 && w > 0 && h > 0) {
                    // Ensure even dimensions (required by most codecs)
                    w = (w + 1) & ~1;
                    h = (h + 1) & ~1;
                    videoEncCtx->width = w;
                    videoEncCtx->height = h;
                    encWidth = w;
                    encHeight = h;
                }
            }
        }

        // CRF and Preset via av_opt_set on priv_data (x264/x265)
        if (opts.video.crf > 0) {
            char crfStr[16];
            snprintf(crfStr, sizeof(crfStr), "%d", opts.video.crf);
            ret = av_opt_set(videoEncCtx->priv_data, "crf", crfStr, 0);
            if (ret < 0) {
                error.message = std::string("Failed to set CRF: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }
        }
        if (!opts.video.preset.empty()) {
            ret = av_opt_set(videoEncCtx->priv_data, "preset", opts.video.preset.c_str(), 0);
            if (ret < 0) {
                error.message = std::string("Failed to set preset: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }
        }

        // Configure video encoder threading
        if (opts.threads > 0) videoEncCtx->thread_count = opts.threads;
        videoEncCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

        // For HW encoding, we need to pass hw_frames_ctx via options
        AVDictionary* encOpts = nullptr;
        if (hwEncode && hwFramesCtx) {
            // hw_frames_ctx is already set on videoEncCtx, no need for options
        }

        if ((ret = avcodec_open2(videoEncCtx, videoEnc, &encOpts)) < 0) {
            av_dict_free(&encOpts);
            error.message = std::string("Failed to open video encoder: ") + av_err_to_string(ret);
            error.operation = "open";
            goto cleanup;
        }
        av_dict_free(&encOpts);

        videoOutStream = avformat_new_stream(ofmtCtx, nullptr);
        if (!videoOutStream) {
            error.message = "Failed to create output video stream";
            error.operation = "open";
            goto cleanup;
        }
        avcodec_parameters_from_context(videoOutStream->codecpar, videoEncCtx);
        videoOutStream->time_base = videoEncCtx->time_base;

        // Setup SwsContext for scaling if dimensions differ (only for SW encoding)
        if (!hwEncode && (encWidth != videoDecCtx->width || encHeight != videoDecCtx->height)) {
            swsCtx = sws_getContext(videoDecCtx->width, videoDecCtx->height, videoDecCtx->pix_fmt,
                                   encWidth, encHeight, videoEncCtx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
        }

        // If HW encoding is enabled but no filters specified, skip filter graph
        // The encoder will handle SW->HW frame transfer internally if supported
        if (hwEncode && hwFramesCtx && opts.video.filters.empty()) {
            // No filter graph needed - encoder handles frame conversion
        }

        // Setup video filter graph if filters are specified
        if (!opts.video.filters.empty()) {
            videoFilterGraph = avfilter_graph_alloc();
            if (!videoFilterGraph) {
                error.message = "Failed to allocate video filter graph";
                error.operation = "open";
                goto cleanup;
            }

            // Create buffer source filter
            const AVFilter* videoBuffersrc = avfilter_get_by_name("buffer");
            if (!videoBuffersrc) {
                error.message = "Video buffer source filter not found";
                error.operation = "open";
                goto cleanup;
            }

            // Use stream's time base for the buffer source
            AVRational srcTimeBase = ifmtCtx->streams[videoStreamIdx]->time_base;
            char args[512];
            snprintf(args, sizeof(args),
                     "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d",
                     videoDecCtx->width, videoDecCtx->height, videoDecCtx->pix_fmt,
                     srcTimeBase.num, srcTimeBase.den);

            ret = avfilter_graph_create_filter(&videoBuffersrcCtx, videoBuffersrc, "src",
                                                args, nullptr, videoFilterGraph);
            if (ret < 0) {
                error.message = std::string("Failed to create video buffer source: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }

            // Create buffer sink filter
            const AVFilter* videoBuffersink = avfilter_get_by_name("buffersink");
            if (!videoBuffersink) {
                error.message = "Video buffer sink filter not found";
                error.operation = "open";
                goto cleanup;
            }

            ret = avfilter_graph_create_filter(&videoBuffersinkCtx, videoBuffersink, "sink",
                                                nullptr, nullptr, videoFilterGraph);
            if (ret < 0) {
                error.message = std::string("Failed to create video buffer sink: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }

            // Parse filter string
            AVFilterInOut* inputs = nullptr;
            AVFilterInOut* outputs = nullptr;
            ret = avfilter_graph_parse2(videoFilterGraph, opts.video.filters.c_str(),
                                        &inputs, &outputs);
            if (ret < 0) {
                error.message = std::string("Failed to parse video filter graph: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }

            // Link buffersrc output to first parsed filter input
            if (inputs) {
                ret = avfilter_link(videoBuffersrcCtx, 0, inputs->filter_ctx, inputs->pad_idx);
                if (ret < 0) {
                    avfilter_inout_free(&inputs);
                    avfilter_inout_free(&outputs);
                    error.message = std::string("Failed to link video filter graph: ") + av_err_to_string(ret);
                    error.operation = "open";
                    goto cleanup;
                }
            }

            // Link last parsed filter output to buffersink input
            if (outputs) {
                AVFilterInOut* cur = outputs;
                while (cur->next) cur = cur->next;
                ret = avfilter_link(cur->filter_ctx, cur->pad_idx, videoBuffersinkCtx, 0);
                if (ret < 0) {
                    avfilter_inout_free(&inputs);
                    avfilter_inout_free(&outputs);
                    error.message = std::string("Failed to link video filter graph: ") + av_err_to_string(ret);
                    error.operation = "open";
                    goto cleanup;
                }
            }

            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);

            // Configure filter graph
            ret = avfilter_graph_config(videoFilterGraph, nullptr);
            if (ret < 0) {
                error.message = std::string("Failed to configure video filter graph: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }

            // When using filter graph, we don't pre-configure encoder dimensions
            // The filter will produce frames with its own output dimensions
            // Note: swsCtx should NOT be used when filter graph is active
            swsCtx = nullptr; // Disable swsCtx when using filter graph
        }
    } else if (videoCopy) {
        videoOutStream = avformat_new_stream(ofmtCtx, nullptr);
        if (!videoOutStream) {
            error.message = "Failed to create output video stream";
            error.operation = "open";
            goto cleanup;
        }
        avcodec_parameters_copy(videoOutStream->codecpar, ifmtCtx->streams[videoStreamIdx]->codecpar);
        videoOutStream->time_base = ifmtCtx->streams[videoStreamIdx]->time_base;
    }

    // Audio encoding pipeline (filter graph only — SWR reserved for streaming path)
    AVCodecContext* audioDecCtx = nullptr;
    AVCodecContext* audioEncCtx = nullptr;
    AVFilterGraph* audioFilterGraph = nullptr;
    AVFilterContext* audioBuffersrcCtx = nullptr;
    AVFilterContext* audioBuffersinkCtx = nullptr;
    bool audioEnabled = (audioStreamIdx >= 0 && !opts.audio.codec.empty() && opts.audio.codec != "copy");
    bool audioCopy = (audioStreamIdx >= 0 && opts.audio.codec == "copy");

    if (audioEnabled) {
        AVStream* audioStream = ifmtCtx->streams[audioStreamIdx];
        const AVCodec* audioDec = avcodec_find_decoder(audioStream->codecpar->codec_id);
        if (!audioDec) {
            error.message = "Audio decoder not found";
            error.operation = "open";
            goto cleanup;
        }

        audioDecCtx = avcodec_alloc_context3(audioDec);
        if (!audioDecCtx) {
            error.message = "Failed to allocate audio decoder context";
            error.operation = "open";
            goto cleanup;
        }
        avcodec_parameters_to_context(audioDecCtx, audioStream->codecpar);
        if (opts.threads > 0) audioDecCtx->thread_count = opts.threads;
        audioDecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if ((ret = avcodec_open2(audioDecCtx, audioDec, nullptr)) < 0) {
            error.message = std::string("Failed to open audio decoder: ") + av_err_to_string(ret);
            error.operation = "open";
            goto cleanup;
        }

        const AVCodec* audioEnc = avcodec_find_encoder_by_name(opts.audio.codec.c_str());
        if (!audioEnc) {
            error.message = std::string("Audio encoder not found: ") + opts.audio.codec;
            error.operation = "open";
            goto cleanup;
        }

        audioEncCtx = avcodec_alloc_context3(audioEnc);
        if (!audioEncCtx) {
            error.message = "Failed to allocate audio encoder context";
            error.operation = "open";
            goto cleanup;
        }

        audioEncCtx->sample_rate = opts.audio.sampleRate > 0 ? opts.audio.sampleRate : audioStream->codecpar->sample_rate;
        if (opts.audio.bitrate > 0) {
            audioEncCtx->bit_rate = opts.audio.bitrate;
        }

        // Channel layout
        if (opts.audio.channels > 0) {
            av_channel_layout_default(&audioEncCtx->ch_layout, opts.audio.channels);
        } else if (audioStream->codecpar->ch_layout.nb_channels > 0) {
            audioEncCtx->ch_layout = audioStream->codecpar->ch_layout;
        } else {
            audioEncCtx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        }

        // Find supported sample format
        audioEncCtx->sample_fmt = find_best_sample_fmt(audioEnc, AV_SAMPLE_FMT_FLTP);

        if ((ret = avcodec_open2(audioEncCtx, audioEnc, nullptr)) < 0) {
            error.message = std::string("Failed to open audio encoder: ") + av_err_to_string(ret);
            error.operation = "open";
            goto cleanup;
        }

        audioOutStream = avformat_new_stream(ofmtCtx, nullptr);
        if (!audioOutStream) {
            error.message = "Failed to create output audio stream";
            error.operation = "open";
            goto cleanup;
        }
        avcodec_parameters_from_context(audioOutStream->codecpar, audioEncCtx);
        audioOutStream->time_base = audioEncCtx->time_base;

        // Always use filter graph for audio in transcode path.
        // FFmpeg's filter graph handles resampling, frame sizing, channel layout,
        // and timestamp propagation internally. This is the correct approach for
        // file-to-file transcoding (Path A). Manual SWR is reserved for the
        // streaming path (Path B) where frame-by-frame control is needed.
        //
        // Graph: abuffer → [user filters] → aformat → abuffersink
        // aformat ensures output matches encoder requirements exactly.
        {
            audioFilterGraph = avfilter_graph_alloc();
            if (!audioFilterGraph) {
                error.message = "Failed to allocate audio filter graph";
                error.operation = "open";
                goto cleanup;
            }

            const AVFilter* audioBuffersrc = avfilter_get_by_name("abuffer");
            if (!audioBuffersrc) {
                error.message = "Audio buffer source filter not found";
                error.operation = "open";
                goto cleanup;
            }

            char srcArgs[512];
            snprintf(srcArgs, sizeof(srcArgs),
                     "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
                     audioDecCtx->time_base.num, audioDecCtx->time_base.den,
                     audioDecCtx->sample_rate,
                     av_get_sample_fmt_name(audioDecCtx->sample_fmt),
                     audioDecCtx->ch_layout.u.mask);

            ret = avfilter_graph_create_filter(&audioBuffersrcCtx, audioBuffersrc, "src",
                                                srcArgs, nullptr, audioFilterGraph);
            if (ret < 0) {
                error.message = std::string("Failed to create audio buffer source: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }

            // aformat filter — ensures output matches encoder requirements
            const AVFilter* aformatFilter = avfilter_get_by_name("aformat");
            if (!aformatFilter) {
                error.message = "aformat filter not found";
                error.operation = "open";
                goto cleanup;
            }

            char fmtArgs[512];
            snprintf(fmtArgs, sizeof(fmtArgs),
                     "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%" PRIx64,
                     av_get_sample_fmt_name(audioEncCtx->sample_fmt),
                     audioEncCtx->sample_rate,
                     audioEncCtx->ch_layout.u.mask);

            AVFilterContext* aformatCtx = nullptr;
            ret = avfilter_graph_create_filter(&aformatCtx, aformatFilter, "aformat",
                                                fmtArgs, nullptr, audioFilterGraph);
            if (ret < 0) {
                error.message = std::string("Failed to create aformat filter: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }

            const AVFilter* audioBuffersink = avfilter_get_by_name("abuffersink");
            if (!audioBuffersink) {
                error.message = "Audio buffer sink filter not found";
                error.operation = "open";
                goto cleanup;
            }

            ret = avfilter_graph_create_filter(&audioBuffersinkCtx, audioBuffersink, "sink",
                                                nullptr, nullptr, audioFilterGraph);
            if (ret < 0) {
                error.message = std::string("Failed to create audio buffer sink: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }

            // asetnsamples — buffers frames to exactly encoder's frame_size
            // Without this, decoders with different native frame sizes (e.g. Opus=960)
            // produce wrong-sized frames that the encoder rejects/warns about
            const AVFilter* asetnsamplesFilter = avfilter_get_by_name("asetnsamples");
            if (!asetnsamplesFilter) {
                error.message = "asetnsamples filter not found";
                error.operation = "open";
                goto cleanup;
            }

            char nsamplesArgs[64];
            snprintf(nsamplesArgs, sizeof(nsamplesArgs), "n=%d", audioEncCtx->frame_size);
            AVFilterContext* asetnsamplesCtx = nullptr;
            ret = avfilter_graph_create_filter(&asetnsamplesCtx, asetnsamplesFilter, "asetnsamples",
                                                nsamplesArgs, nullptr, audioFilterGraph);
            if (ret < 0) {
                error.message = std::string("Failed to create asetnsamples filter: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }

            // Build the chain: src → [user filters] → aformat → asetnsamples → sink
            if (!opts.audio.filters.empty()) {
                AVFilterInOut* inputs = nullptr;
                AVFilterInOut* outputs = nullptr;
                ret = avfilter_graph_parse2(audioFilterGraph, opts.audio.filters.c_str(),
                                            &inputs, &outputs);
                if (ret < 0) {
                    avfilter_inout_free(&inputs);
                    avfilter_inout_free(&outputs);
                    error.message = std::string("Failed to parse audio filter graph: ") + av_err_to_string(ret);
                    error.operation = "open";
                    goto cleanup;
                }

                // src → first user filter input
                if (inputs) {
                    ret = avfilter_link(audioBuffersrcCtx, 0, inputs->filter_ctx, inputs->pad_idx);
                    if (ret < 0) {
                        avfilter_inout_free(&inputs);
                        avfilter_inout_free(&outputs);
                        error.message = std::string("Failed to link audio filter: src → user") + av_err_to_string(ret);
                        error.operation = "open";
                        goto cleanup;
                    }
                }

                // last user filter output → aformat
                if (outputs) {
                    AVFilterInOut* cur = outputs;
                    while (cur->next) cur = cur->next;
                    ret = avfilter_link(cur->filter_ctx, cur->pad_idx, aformatCtx, 0);
                    if (ret < 0) {
                        avfilter_inout_free(&inputs);
                        avfilter_inout_free(&outputs);
                        error.message = std::string("Failed to link audio filter: user → aformat: ") + av_err_to_string(ret);
                        error.operation = "open";
                        goto cleanup;
                    }
                }

                avfilter_inout_free(&inputs);
                avfilter_inout_free(&outputs);
            } else {
                // No user filters: src → aformat directly
                ret = avfilter_link(audioBuffersrcCtx, 0, aformatCtx, 0);
                if (ret < 0) {
                    error.message = std::string("Failed to link audio: src → aformat: ") + av_err_to_string(ret);
                    error.operation = "open";
                    goto cleanup;
                }
            }

            // aformat → asetnsamples → sink
            ret = avfilter_link(aformatCtx, 0, asetnsamplesCtx, 0);
            if (ret < 0) {
                error.message = std::string("Failed to link audio: aformat → asetnsamples: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }

            ret = avfilter_link(asetnsamplesCtx, 0, audioBuffersinkCtx, 0);
            if (ret < 0) {
                error.message = std::string("Failed to link audio: asetnsamples → sink: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }

            ret = avfilter_graph_config(audioFilterGraph, nullptr);
            if (ret < 0) {
                error.message = std::string("Failed to configure audio filter graph: ") + av_err_to_string(ret);
                error.operation = "open";
                goto cleanup;
            }
        }
    } else if (audioCopy) {
        audioOutStream = avformat_new_stream(ofmtCtx, nullptr);
        if (!audioOutStream) {
            error.message = "Failed to create output audio stream";
            error.operation = "open";
            goto cleanup;
        }
        avcodec_parameters_copy(audioOutStream->codecpar, ifmtCtx->streams[audioStreamIdx]->codecpar);
        audioOutStream->time_base = ifmtCtx->streams[audioStreamIdx]->time_base;
    }

    // Open output file
    if (!(ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&ofmtCtx->pb, outputPath, AVIO_FLAG_WRITE)) < 0) {
            error.message = std::string("Failed to open output file: ") + av_err_to_string(ret);
            error.operation = "open";
            goto cleanup;
        }
    }

    if ((ret = avformat_write_header(ofmtCtx, nullptr)) < 0) {
        error.message = std::string("Failed to write header: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    double totalDuration = 0.0;
    if (ifmtCtx->duration != AV_NOPTS_VALUE) {
        totalDuration = static_cast<double>(ifmtCtx->duration) / AV_TIME_BASE;
    }

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt) {
        error.message = "Failed to allocate frame/packet";
        error.operation = "decode";
        goto cleanup;
    }

    // Allocate scaled frame for video
    if (videoEnabled) {
        scaledFrame = av_frame_alloc();
        scaledFrame->format = videoEncCtx->pix_fmt;
        scaledFrame->width = videoEncCtx->width;
        scaledFrame->height = videoEncCtx->height;
        av_frame_get_buffer(scaledFrame, 0);
    }

    int64_t startTime = av_gettime();
    int64_t totalFrames = 0;
    int64_t totalAudioSamples = 0;
    int64_t audioPtsCounter = 0; // monotonic PTS counter for audio encoder
    int dupFrames = 0, dropFrames = 0;
    double lastProgressTime = 0.0;
    const double progressInterval = 0.1;

    // Transcode loop
    while (av_read_frame(ifmtCtx, pkt) >= 0) {
        int streamIdx = pkt->stream_index;

        if (streamIdx == videoStreamIdx && videoEnabled) {
            ret = avcodec_send_packet(videoDecCtx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) continue;

            while (avcodec_receive_frame(videoDecCtx, frame) >= 0) {
                AVFrame* encFrame = frame;

                // Process through filter graph if configured
                if (videoFilterGraph && videoBuffersrcCtx && videoBuffersinkCtx) {
                    ret = av_buffersrc_add_frame_flags(videoBuffersrcCtx, frame, 0);
                    if (ret < 0) {
                        av_frame_unref(frame);
                        continue;
                    }

                    AVFrame* filteredFrame = av_frame_alloc();
                    while (true) {
                        ret = av_buffersink_get_frame(videoBuffersinkCtx, filteredFrame);
                        if (ret < 0) break;

                        // Use filtered frame for encoding
                        ret = avcodec_send_frame(videoEncCtx, filteredFrame);
                        if (ret < 0) {
                            av_frame_unref(filteredFrame);
                            continue;
                        }

                        while (ret >= 0) {
                            ret = avcodec_receive_packet(videoEncCtx, pkt);
                            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                            pkt->stream_index = videoOutStream->index;
                            av_interleaved_write_frame(ofmtCtx, pkt);
                            totalFrames++;
                        }
                        av_frame_unref(filteredFrame);
                    }
                    av_frame_unref(frame);
                } else {
                    // Scale if needed (no filter graph)
                    if (swsCtx) {
                        sws_scale(swsCtx, frame->data, frame->linesize, 0, frame->height,
                                  scaledFrame->data, scaledFrame->linesize);
                        encFrame = scaledFrame;
                    }

                    ret = avcodec_send_frame(videoEncCtx, encFrame);
                    if (ret < 0) {
                        if (encFrame == scaledFrame) av_frame_unref(encFrame);
                        av_frame_unref(frame);
                        continue;
                    }

                    while (ret >= 0) {
                        ret = avcodec_receive_packet(videoEncCtx, pkt);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                        pkt->stream_index = videoOutStream->index;
                        av_interleaved_write_frame(ofmtCtx, pkt);
                        totalFrames++;
                    }

                    if (encFrame == scaledFrame) av_frame_unref(encFrame);
                    av_frame_unref(frame);
                }
            }

            // Progress
            double currentTime = (av_gettime() - startTime) / 1000000.0;
            if (progressCallback && (currentTime - lastProgressTime) >= progressInterval) {
                TranscodeProgress prog = {};
                prog.time = currentTime;
                prog.percent = totalDuration > 0 ? (currentTime / totalDuration) * 100.0 : 0.0;
                prog.speed = currentTime > 0 ? currentTime / (currentTime + 0.001) : 0.0;
                prog.frames = totalFrames;
                prog.fps = currentTime > 0 ? totalFrames / currentTime : 0.0;
                prog.audioFrames = totalAudioSamples;
                prog.estimatedDuration = totalDuration;
                prog.eta = (prog.speed > 0) ? (totalDuration - currentTime) / prog.speed : 0.0;
                progressCallback(prog);
                lastProgressTime = currentTime;
            }
        } else if (streamIdx == videoStreamIdx && videoCopy) {
            pkt->stream_index = videoOutStream->index;
            av_interleaved_write_frame(ofmtCtx, pkt);
            av_packet_unref(pkt);
            totalFrames++;
        } else if (streamIdx == audioStreamIdx && audioEnabled) {
            ret = avcodec_send_packet(audioDecCtx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) continue;

            while (avcodec_receive_frame(audioDecCtx, frame) >= 0) {
                ret = av_buffersrc_add_frame_flags(audioBuffersrcCtx, frame, 0);
                av_frame_unref(frame);
                if (ret < 0) continue;

                AVFrame* filteredFrame = av_frame_alloc();
                while (true) {
                    ret = av_buffersink_get_frame(audioBuffersinkCtx, filteredFrame);
                    if (ret < 0) break;

                    // Generate monotonic PTS from sample count — avoids all timestamp
                    // issues from codec priming, format conversion, and filter buffering.
                    // PTS is in encoder time_base (sample-rate based).
                    filteredFrame->pts = audioPtsCounter;
                    audioPtsCounter += filteredFrame->nb_samples;
                    totalAudioSamples += filteredFrame->nb_samples;

                    ret = avcodec_send_frame(audioEncCtx, filteredFrame);
                    if (ret < 0) {
                        av_frame_unref(filteredFrame);
                        continue;
                    }

                    while (ret >= 0) {
                        ret = avcodec_receive_packet(audioEncCtx, pkt);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                        pkt->stream_index = audioOutStream->index;
                        av_interleaved_write_frame(ofmtCtx, pkt);
                    }
                    av_frame_unref(filteredFrame);
                }
                av_frame_free(&filteredFrame);
            }
        } else if (streamIdx == audioStreamIdx && audioCopy) {
            pkt->stream_index = audioOutStream->index;
            av_interleaved_write_frame(ofmtCtx, pkt);
            av_packet_unref(pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    // Flush audio filter graph — drain remaining buffered samples from asetnsamples
    if (audioEnabled && audioBuffersrcCtx && audioBuffersinkCtx) {
        av_buffersrc_add_frame(audioBuffersrcCtx, nullptr);
        AVFrame* flushFrame = av_frame_alloc();
        while (av_buffersink_get_frame(audioBuffersinkCtx, flushFrame) >= 0) {
            flushFrame->pts = audioPtsCounter;
            audioPtsCounter += flushFrame->nb_samples;
            totalAudioSamples += flushFrame->nb_samples;
            if (avcodec_send_frame(audioEncCtx, flushFrame) >= 0) {
                while (avcodec_receive_packet(audioEncCtx, pkt) >= 0) {
                    pkt->stream_index = audioOutStream->index;
                    av_interleaved_write_frame(ofmtCtx, pkt);
                }
            }
            av_frame_unref(flushFrame);
        }
        av_frame_free(&flushFrame);
    }

    // Flush encoders
    if (videoEnabled && videoEncCtx) {
        avcodec_send_frame(videoEncCtx, nullptr);
        while (avcodec_receive_packet(videoEncCtx, pkt) >= 0) {
            pkt->stream_index = videoOutStream->index;
            av_interleaved_write_frame(ofmtCtx, pkt);
        }
    }
    if (audioEnabled && audioEncCtx) {
        avcodec_send_frame(audioEncCtx, nullptr);
        while (avcodec_receive_packet(audioEncCtx, pkt) >= 0) {
            pkt->stream_index = audioOutStream->index;
            av_interleaved_write_frame(ofmtCtx, pkt);
        }
    }

    av_write_trailer(ofmtCtx);

    int64_t endTime = av_gettime();
    result.duration = totalDuration;
    result.frames = totalFrames;
    result.audioFrames = totalAudioSamples;
    result.size = ofmtCtx->pb ? avio_size(ofmtCtx->pb) : 0;
    result.bitrate = result.size * 8 / (totalDuration > 0 ? totalDuration : 1);
    result.speed = (endTime - startTime) / 1000000.0 / (totalDuration > 0 ? totalDuration : 1);
    result.timeMs = (endTime - startTime) / 1000;
    result.dupFrames = dupFrames;
    result.dropFrames = dropFrames;

    success = true;

cleanup:
    if (scaledFrame) av_frame_free(&scaledFrame);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (videoDecCtx) avcodec_free_context(&videoDecCtx);
    if (videoEncCtx) avcodec_free_context(&videoEncCtx);
    if (audioDecCtx) avcodec_free_context(&audioDecCtx);
    if (audioEncCtx) avcodec_free_context(&audioEncCtx);
    if (swsCtx) sws_freeContext(swsCtx);
    if (videoFilterGraph) avfilter_graph_free(&videoFilterGraph);
    if (audioFilterGraph) avfilter_graph_free(&audioFilterGraph);
    if (ofmtCtx && !(ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&ofmtCtx->pb);
    }
    if (ofmtCtx) avformat_free_context(ofmtCtx);
    if (ifmtCtx) avformat_close_input(&ifmtCtx);

    return success;
}

bool FFmpegProcessor::remux(const char* inputPath, const char* outputPath,
                            TranscodeProgressCallback progressCallback,
                            TranscodeResult& result,
                            TranscodeError& error) {
    result = TranscodeResult();
    error = TranscodeError();
    error.code = 0;

    AVFormatContext* ifmtCtx = nullptr;
    AVFormatContext* ofmtCtx = nullptr;
    AVPacket* pkt = nullptr;

    int ret = 0;
    bool success = false;

    if ((ret = avformat_open_input(&ifmtCtx, inputPath, nullptr, nullptr)) < 0) {
        error.message = std::string("Failed to open input: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    if ((ret = avformat_find_stream_info(ifmtCtx, nullptr)) < 0) {
        error.message = std::string("Failed to find stream info: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    if ((ret = avformat_alloc_output_context2(&ofmtCtx, nullptr, nullptr, outputPath)) < 0) {
        error.message = std::string("Failed to create output context: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    for (unsigned int i = 0; i < ifmtCtx->nb_streams; i++) {
        AVStream* inStream = ifmtCtx->streams[i];
        AVStream* outStream = avformat_new_stream(ofmtCtx, nullptr);
        if (!outStream) {
            error.message = "Failed to create output stream";
            error.operation = "open";
            goto cleanup;
        }
        avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        outStream->time_base = inStream->time_base;
    }

    if (!(ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&ofmtCtx->pb, outputPath, AVIO_FLAG_WRITE)) < 0) {
            error.message = std::string("Failed to open output file: ") + av_err_to_string(ret);
            error.operation = "open";
            goto cleanup;
        }
    }

    if ((ret = avformat_write_header(ofmtCtx, nullptr)) < 0) {
        error.message = std::string("Failed to write header: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    double totalDuration = 0.0;
    if (ifmtCtx->duration != AV_NOPTS_VALUE) {
        totalDuration = static_cast<double>(ifmtCtx->duration) / AV_TIME_BASE;
    }

    pkt = av_packet_alloc();
    int64_t startTime = av_gettime();
    int64_t totalBytes = 0;
    double lastProgressTime = 0.0;
    const double progressInterval = 0.1;

    while (av_read_frame(ifmtCtx, pkt) >= 0) {
        AVStream* inStream = ifmtCtx->streams[pkt->stream_index];
        AVStream* outStream = ofmtCtx->streams[pkt->stream_index];

        pkt->stream_index = outStream->index;
        pkt->pos = -1;

        if ((ret = av_interleaved_write_frame(ofmtCtx, pkt)) < 0) {
            error.message = std::string("Failed to write packet: ") + av_err_to_string(ret);
            error.operation = "write";
            error.timestamp = pkt->pts * av_q2d(inStream->time_base);
            av_packet_unref(pkt);
            goto cleanup;
        }

        totalBytes += pkt->size;

        double currentTime = (av_gettime() - startTime) / 1000000.0;
        if (progressCallback && (currentTime - lastProgressTime) >= progressInterval) {
            TranscodeProgress prog = {};
            prog.time = currentTime;
            prog.percent = totalDuration > 0 ? (currentTime / totalDuration) * 100.0 : 0.0;
            prog.speed = currentTime > 0 ? currentTime / (currentTime + 0.001) : 0.0;
            prog.size = totalBytes;
            prog.estimatedDuration = totalDuration;
            prog.eta = (prog.speed > 0) ? (totalDuration - currentTime) / prog.speed : 0.0;
            progressCallback(prog);
            lastProgressTime = currentTime;
        }

        av_packet_unref(pkt);
    }

    av_write_trailer(ofmtCtx);

    int64_t endTime = av_gettime();
    result.duration = totalDuration;
    result.size = totalBytes;
    result.bitrate = result.size * 8 / (totalDuration > 0 ? totalDuration : 1);
    result.speed = (endTime - startTime) / 1000000.0 / (totalDuration > 0 ? totalDuration : 1);
    result.timeMs = (endTime - startTime) / 1000;
    success = true;

cleanup:
    if (pkt) av_packet_free(&pkt);
    if (ofmtCtx && !(ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&ofmtCtx->pb);
    }
    if (ofmtCtx) avformat_free_context(ofmtCtx);
    if (ifmtCtx) avformat_close_input(&ifmtCtx);

    return success;
}

// Concat - join multiple files into one (stream copy)
bool FFmpegProcessor::concat(const char** inputPaths, int numInputs, const char* outputPath,
                            TranscodeProgressCallback progressCallback,
                            TranscodeResult& result,
                            TranscodeError& error) {
    result = TranscodeResult();
    error = TranscodeError();
    error.code = 0;

    AVFormatContext* ifmtCtx = nullptr;
    AVFormatContext* ofmtCtx = nullptr;
    AVPacket* pkt = av_packet_alloc();
    int ret = 0;
    bool success = false;

    // Open first input to get format info
    if ((ret = avformat_open_input(&ifmtCtx, inputPaths[0], nullptr, nullptr)) < 0) {
        error.message = std::string("Failed to open first input: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    // Create output context based on first input's format
    if ((ret = avformat_alloc_output_context2(&ofmtCtx, nullptr, nullptr, outputPath)) < 0) {
        error.message = std::string("Failed to create output context: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    // Copy streams from first input
    for (unsigned int i = 0; i < ifmtCtx->nb_streams; i++) {
        AVStream* inStream = ifmtCtx->streams[i];
        AVStream* outStream = avformat_new_stream(ofmtCtx, nullptr);
        if (!outStream) {
            error.message = "Failed to create output stream";
            error.operation = "open";
            goto cleanup;
        }
        avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        outStream->time_base = inStream->time_base;
    }

    // Close first input - will reopen each file during concat
    avformat_close_input(&ifmtCtx);
    ifmtCtx = nullptr;

    // Open output file
    if (!(ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&ofmtCtx->pb, outputPath, AVIO_FLAG_WRITE)) < 0) {
            error.message = std::string("Failed to open output file: ") + av_err_to_string(ret);
            error.operation = "open";
            goto cleanup;
        }
    }

    if ((ret = avformat_write_header(ofmtCtx, nullptr)) < 0) {
        error.message = std::string("Failed to write header: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    // Calculate total duration
    double totalDuration = 0.0;
    for (int f = 0; f < numInputs; f++) {
        double dur = probeDuration(inputPaths[f]);
        totalDuration += dur;
    }

    int64_t startTime = av_gettime();
    int64_t totalBytes = 0;
    double currentTime = 0.0;
    double lastProgressTime = 0.0;
    const double progressInterval = 0.1;
    double processedDuration = 0.0;

    // Process each input file
    for (int fileIdx = 0; fileIdx < numInputs; fileIdx++) {
        if ((ret = avformat_open_input(&ifmtCtx, inputPaths[fileIdx], nullptr, nullptr)) < 0) {
            error.message = std::string("Failed to open input file ") + std::to_string(fileIdx) + ": " + av_err_to_string(ret);
            error.operation = "open";
            goto cleanup;
        }

        // Get duration of this file
        double fileDuration = 0.0;
        if (ifmtCtx->duration != AV_NOPTS_VALUE) {
            fileDuration = static_cast<double>(ifmtCtx->duration) / AV_TIME_BASE;
        }

        // Reset packet
        av_packet_unref(pkt);

        while (av_read_frame(ifmtCtx, pkt) >= 0) {
            AVStream* inStream = ifmtCtx->streams[pkt->stream_index];
            AVStream* outStream = ofmtCtx->streams[pkt->stream_index];

            // Adjust timestamps - offset by processed duration
            if (pkt->pts != AV_NOPTS_VALUE) {
                int64_t timeOffset = static_cast<int64_t>(processedDuration * AV_TIME_BASE);
                pkt->pts += timeOffset;
                pkt->dts += timeOffset;
            }
            pkt->stream_index = outStream->index;
            pkt->pos = -1;

            if ((ret = av_interleaved_write_frame(ofmtCtx, pkt)) < 0) {
                error.message = std::string("Failed to write packet: ") + av_err_to_string(ret);
                error.operation = "write";
                error.timestamp = pkt->pts * av_q2d(inStream->time_base) - processedDuration;
                error.stream = pkt->stream_index;
                av_packet_unref(pkt);
                goto cleanup;
            }

            totalBytes += pkt->size;
            av_packet_unref(pkt);
        }

        processedDuration += fileDuration;

        // Progress callback
        currentTime = (av_gettime() - startTime) / 1000000.0;
        if (progressCallback && (currentTime - lastProgressTime) >= progressInterval) {
            TranscodeProgress prog = {};
            prog.time = currentTime;
            prog.percent = totalDuration > 0 ? (processedDuration / totalDuration) * 100.0 : 0.0;
            prog.speed = currentTime > 0 ? processedDuration / currentTime : 0.0;
            prog.size = totalBytes;
            prog.estimatedDuration = totalDuration;
            prog.eta = (prog.speed > 0) ? (totalDuration - processedDuration) / prog.speed : 0.0;
            progressCallback(prog);
            lastProgressTime = currentTime;
        }

        avformat_close_input(&ifmtCtx);
        ifmtCtx = nullptr;
    }

    av_write_trailer(ofmtCtx);

    int64_t endTime = av_gettime();
    result.duration = totalDuration;
    result.size = totalBytes;
    result.bitrate = result.size * 8 / (totalDuration > 0 ? totalDuration : 1);
    result.speed = (endTime - startTime) / 1000000.0 / (totalDuration > 0 ? totalDuration : 1);
    result.timeMs = (endTime - startTime) / 1000;
    success = true;

cleanup:
    if (pkt) av_packet_free(&pkt);
    if (ifmtCtx) avformat_close_input(&ifmtCtx);
    if (ofmtCtx && !(ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&ofmtCtx->pb);
    }
    if (ofmtCtx) avformat_free_context(ofmtCtx);

    return success;
}

// ExtractStream - extract a single stream to a new file
bool FFmpegProcessor::extractStream(const char* inputPath, const char* outputPath, int streamIndex,
                                  TranscodeProgressCallback progressCallback,
                                  TranscodeResult& result,
                                  TranscodeError& error) {
    result = TranscodeResult();
    error = TranscodeError();
    error.code = 0;

    AVFormatContext* ifmtCtx = nullptr;
    AVFormatContext* ofmtCtx = nullptr;
    AVPacket* pkt = av_packet_alloc();
    int ret = 0;
    bool success = false;

    if ((ret = avformat_open_input(&ifmtCtx, inputPath, nullptr, nullptr)) < 0) {
        error.message = std::string("Failed to open input: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    if ((ret = avformat_find_stream_info(ifmtCtx, nullptr)) < 0) {
        error.message = std::string("Failed to find stream info: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    if (streamIndex < 0 || streamIndex >= static_cast<int>(ifmtCtx->nb_streams)) {
        error.message = "Invalid stream index";
        error.operation = "open";
        goto cleanup;
    }

    AVStream* inStream = ifmtCtx->streams[streamIndex];
    AVMediaType codecType = inStream->codecpar->codec_type;

    if ((ret = avformat_alloc_output_context2(&ofmtCtx, nullptr, nullptr, outputPath)) < 0) {
        error.message = std::string("Failed to create output context: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    AVStream* outStream = avformat_new_stream(ofmtCtx, nullptr);
    if (!outStream) {
        error.message = "Failed to create output stream";
        error.operation = "open";
        goto cleanup;
    }
    avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
    outStream->time_base = inStream->time_base;

    if (!(ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&ofmtCtx->pb, outputPath, AVIO_FLAG_WRITE)) < 0) {
            error.message = std::string("Failed to open output file: ") + av_err_to_string(ret);
            error.operation = "open";
            goto cleanup;
        }
    }

    if ((ret = avformat_write_header(ofmtCtx, nullptr)) < 0) {
        error.message = std::string("Failed to write header: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    double totalDuration = 0.0;
    if (ifmtCtx->duration != AV_NOPTS_VALUE) {
        totalDuration = static_cast<double>(ifmtCtx->duration) / AV_TIME_BASE;
    }

    int64_t startTime = av_gettime();
    int64_t totalBytes = 0;
    double lastProgressTime = 0.0;
    const double progressInterval = 0.1;

    while (av_read_frame(ifmtCtx, pkt) >= 0) {
        if (pkt->stream_index == streamIndex) {
            pkt->stream_index = outStream->index;
            pkt->pos = -1;

            if ((ret = av_interleaved_write_frame(ofmtCtx, pkt)) < 0) {
                error.message = std::string("Failed to write packet: ") + av_err_to_string(ret);
                error.operation = "write";
                error.timestamp = pkt->pts * av_q2d(inStream->time_base);
                av_packet_unref(pkt);
                goto cleanup;
            }

            totalBytes += pkt->size;
        }

        av_packet_unref(pkt);

        double currentTime = (av_gettime() - startTime) / 1000000.0;
        if (progressCallback && (currentTime - lastProgressTime) >= progressInterval) {
            TranscodeProgress prog = {};
            prog.time = currentTime;
            prog.percent = totalDuration > 0 ? (currentTime / totalDuration) * 100.0 : 0.0;
            prog.speed = currentTime > 0 ? currentTime / (currentTime + 0.001) : 0.0;
            prog.size = totalBytes;
            prog.estimatedDuration = totalDuration;
            prog.eta = (prog.speed > 0) ? (totalDuration - currentTime) / prog.speed : 0.0;
            progressCallback(prog);
            lastProgressTime = currentTime;
        }
    }

    av_write_trailer(ofmtCtx);

    int64_t endTime = av_gettime();
    result.duration = totalDuration;
    result.size = totalBytes;
    result.bitrate = result.size * 8 / (totalDuration > 0 ? totalDuration : 1);
    result.speed = (endTime - startTime) / 1000000.0 / (totalDuration > 0 ? totalDuration : 1);
    result.timeMs = (endTime - startTime) / 1000;
    success = true;

cleanup:
    if (pkt) av_packet_free(&pkt);
    if (ofmtCtx && !(ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&ofmtCtx->pb);
    }
    if (ofmtCtx) avformat_free_context(ofmtCtx);
    if (ifmtCtx) avformat_close_input(&ifmtCtx);

    return success;
}

bool FFmpegProcessor::extractAudio(const char* inputPath, const char* outputPath,
                                    const TranscodeOptions::AudioOpts& audioOpts,
                                    TranscodeProgressCallback progressCallback,
                                    TranscodeResult& result,
                                    TranscodeError& error) {
    result = TranscodeResult();
    error = TranscodeError();
    error.code = 0;

    AVFormatContext* ifmtCtx = nullptr;
    AVFormatContext* ofmtCtx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int ret = 0;
    bool success = false;
    std::string codecName = audioOpts.codec;

    if ((ret = avformat_open_input(&ifmtCtx, inputPath, nullptr, nullptr)) < 0) {
        error.message = std::string("Failed to open input: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }
    if ((ret = avformat_find_stream_info(ifmtCtx, nullptr)) < 0) {
        error.message = std::string("Failed to find stream info: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    int audioStreamIdx = -1;
    for (unsigned int i = 0; i < ifmtCtx->nb_streams; i++) {
        if (ifmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIdx = static_cast<int>(i);
            break;
        }
    }
    if (audioStreamIdx < 0) {
        error.message = "No audio stream found in input";
        error.operation = "open";
        goto cleanup;
    }
    AVStream* audioStream = ifmtCtx->streams[audioStreamIdx];

    if ((ret = avformat_alloc_output_context2(&ofmtCtx, nullptr, nullptr, outputPath)) < 0) {
        error.message = std::string("Failed to create output context: ") + av_err_to_string(ret);
        error.operation = "open";
        goto cleanup;
    }

    if (codecName.empty()) {
        std::string outPath(outputPath);
        size_t dot = outPath.rfind('.');
        if (dot != std::string::npos) {
            std::string ext = outPath.substr(dot + 1);
            if (ext == "wav") codecName = "pcm_s16le";
            else if (ext == "mp3") codecName = "libmp3lame";
            else if (ext == "aac") codecName = "aac";
            else if (ext == "flac") codecName = "flac";
            else if (ext == "ogg") codecName = "libvorbis";
            else if (ext == "opus") codecName = "libopus";
            else if (ext == "m4a") codecName = "aac";
            else codecName = "aac";
        } else {
            codecName = "aac";
        }
    }

    AVCodecContext* audioDecCtx = nullptr;
    AVCodecContext* audioEncCtx = nullptr;
    AVFilterGraph* audioFilterGraph = nullptr;
    AVFilterContext* audioBuffersrcCtx = nullptr;
    AVFilterContext* audioBuffersinkCtx = nullptr;

    const AVCodec* audioDec = avcodec_find_decoder(audioStream->codecpar->codec_id);
    if (!audioDec) { error.message = "Audio decoder not found"; error.operation = "open"; goto cleanup; }
    audioDecCtx = avcodec_alloc_context3(audioDec);
    if (!audioDecCtx) { error.message = "Failed to allocate audio decoder context"; error.operation = "open"; goto cleanup; }
    avcodec_parameters_to_context(audioDecCtx, audioStream->codecpar);
    audioDecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if ((ret = avcodec_open2(audioDecCtx, audioDec, nullptr)) < 0) {
        error.message = std::string("Failed to open audio decoder: ") + av_err_to_string(ret);
        error.operation = "open"; goto cleanup;
    }

    const AVCodec* audioEnc = avcodec_find_encoder_by_name(codecName.c_str());
    if (!audioEnc) { error.message = std::string("Audio encoder not found: ") + codecName; error.operation = "open"; goto cleanup; }
    audioEncCtx = avcodec_alloc_context3(audioEnc);
    if (!audioEncCtx) { error.message = "Failed to allocate audio encoder context"; error.operation = "open"; goto cleanup; }
    audioEncCtx->sample_rate = audioOpts.sampleRate > 0 ? audioOpts.sampleRate : audioStream->codecpar->sample_rate;
    if (audioOpts.bitrate > 0) audioEncCtx->bit_rate = audioOpts.bitrate;
    if (audioOpts.channels > 0) av_channel_layout_default(&audioEncCtx->ch_layout, audioOpts.channels);
    else if (audioStream->codecpar->ch_layout.nb_channels > 0) audioEncCtx->ch_layout = audioStream->codecpar->ch_layout;
    else audioEncCtx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    audioEncCtx->sample_fmt = find_best_sample_fmt(audioEnc, AV_SAMPLE_FMT_FLTP);
    if ((ret = avcodec_open2(audioEncCtx, audioEnc, nullptr)) < 0) {
        error.message = std::string("Failed to open audio encoder: ") + av_err_to_string(ret);
        error.operation = "open"; goto cleanup;
    }

    AVStream* audioOutStream = avformat_new_stream(ofmtCtx, nullptr);
    if (!audioOutStream) { error.message = "Failed to create output audio stream"; error.operation = "open"; goto cleanup; }
    avcodec_parameters_from_context(audioOutStream->codecpar, audioEncCtx);
    audioOutStream->time_base = audioEncCtx->time_base;

    audioFilterGraph = avfilter_graph_alloc();
    if (!audioFilterGraph) { error.message = "Failed to allocate audio filter graph"; error.operation = "open"; goto cleanup; }

    const AVFilter* audioBuffersrc = avfilter_get_by_name("abuffer");
    if (!audioBuffersrc) { error.message = "Audio buffer source filter not found"; error.operation = "open"; goto cleanup; }
    char srcArgs[512];
    snprintf(srcArgs, sizeof(srcArgs),
             "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
             audioDecCtx->time_base.num, audioDecCtx->time_base.den,
             audioDecCtx->sample_rate,
             av_get_sample_fmt_name(audioDecCtx->sample_fmt),
             audioDecCtx->ch_layout.u.mask);
    ret = avfilter_graph_create_filter(&audioBuffersrcCtx, audioBuffersrc, "src", srcArgs, nullptr, audioFilterGraph);
    if (ret < 0) { error.message = std::string("Failed to create audio buffer source: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup; }

    const AVFilter* aformatFilter = avfilter_get_by_name("aformat");
    if (!aformatFilter) { error.message = "aformat filter not found"; error.operation = "open"; goto cleanup; }
    char fmtArgs[512];
    snprintf(fmtArgs, sizeof(fmtArgs),
             "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%" PRIx64,
             av_get_sample_fmt_name(audioEncCtx->sample_fmt),
             audioEncCtx->sample_rate,
             audioEncCtx->ch_layout.u.mask);
    AVFilterContext* aformatCtx = nullptr;
    ret = avfilter_graph_create_filter(&aformatCtx, aformatFilter, "aformat", fmtArgs, nullptr, audioFilterGraph);
    if (ret < 0) { error.message = std::string("Failed to create aformat filter: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup; }

    const AVFilter* audioBuffersink = avfilter_get_by_name("abuffersink");
    if (!audioBuffersink) { error.message = "Audio buffer sink filter not found"; error.operation = "open"; goto cleanup; }
    ret = avfilter_graph_create_filter(&audioBuffersinkCtx, audioBuffersink, "sink", nullptr, nullptr, audioFilterGraph);
    if (ret < 0) { error.message = std::string("Failed to create audio buffer sink: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup; }

    const AVFilter* asetnsamplesFilter = avfilter_get_by_name("asetnsamples");
    if (!asetnsamplesFilter) { error.message = "asetnsamples filter not found"; error.operation = "open"; goto cleanup; }
    AVFilterContext* asetnsamplesCtx = nullptr;
    if (audioEncCtx->frame_size > 0) {
        char nsamplesArgs[64];
        snprintf(nsamplesArgs, sizeof(nsamplesArgs), "n=%d", audioEncCtx->frame_size);
        ret = avfilter_graph_create_filter(&asetnsamplesCtx, asetnsamplesFilter, "asetnsamples", nsamplesArgs, nullptr, audioFilterGraph);
        if (ret < 0) { error.message = std::string("Failed to create asetnsamples filter: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup; }
    }

    if (!audioOpts.filters.empty()) {
        AVFilterInOut* inputs = nullptr;
        AVFilterInOut* outputs = nullptr;
        ret = avfilter_graph_parse2(audioFilterGraph, audioOpts.filters.c_str(), &inputs, &outputs);
        if (ret < 0) { avfilter_inout_free(&inputs); avfilter_inout_free(&outputs); error.message = std::string("Failed to parse audio filter: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup; }
        if (inputs) { ret = avfilter_link(audioBuffersrcCtx, 0, inputs->filter_ctx, inputs->pad_idx); if (ret < 0) { avfilter_inout_free(&inputs); avfilter_inout_free(&outputs); error.message = std::string("Failed to link audio filter: src → user") + av_err_to_string(ret); error.operation = "open"; goto cleanup; } }
        if (outputs) { AVFilterInOut* cur = outputs; while (cur->next) cur = cur->next; ret = avfilter_link(cur->filter_ctx, cur->pad_idx, aformatCtx, 0); if (ret < 0) { avfilter_inout_free(&inputs); avfilter_inout_free(&outputs); error.message = std::string("Failed to link audio filter: user → aformat: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup; } }
        avfilter_inout_free(&inputs); avfilter_inout_free(&outputs);
    } else {
        ret = avfilter_link(audioBuffersrcCtx, 0, aformatCtx, 0);
        if (ret < 0) { error.message = std::string("Failed to link audio: src → aformat: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup; }
    }
    if (asetnsamplesCtx) {
        ret = avfilter_link(aformatCtx, 0, asetnsamplesCtx, 0);
        if (ret < 0) { error.message = std::string("Failed to link audio: aformat → asetnsamples: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup; }
        ret = avfilter_link(asetnsamplesCtx, 0, audioBuffersinkCtx, 0);
        if (ret < 0) { error.message = std::string("Failed to link audio: asetnsamples → sink: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup; }
    } else {
        ret = avfilter_link(aformatCtx, 0, audioBuffersinkCtx, 0);
        if (ret < 0) { error.message = std::string("Failed to link audio: aformat → sink: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup; }
    }
    ret = avfilter_graph_config(audioFilterGraph, nullptr);
    if (ret < 0) { error.message = std::string("Failed to configure audio filter graph: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup; }

    if (!(ofmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&ofmtCtx->pb, outputPath, AVIO_FLAG_WRITE)) < 0) {
            error.message = std::string("Failed to open output file: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup;
        }
    }
    if ((ret = avformat_write_header(ofmtCtx, nullptr)) < 0) {
        error.message = std::string("Failed to write header: ") + av_err_to_string(ret); error.operation = "open"; goto cleanup;
    }

    double totalDuration = 0.0;
    if (ifmtCtx->duration != AV_NOPTS_VALUE) totalDuration = static_cast<double>(ifmtCtx->duration) / AV_TIME_BASE;

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt) { error.message = "Failed to allocate frame/packet"; error.operation = "decode"; goto cleanup; }

    int64_t startTime = av_gettime();
    int64_t totalAudioSamples = 0;
    int64_t audioPtsCounter = 0;
    double lastProgressTime = 0.0;
    const double progressInterval = 0.1;

    while (av_read_frame(ifmtCtx, pkt) >= 0) {
        if (pkt->stream_index != audioStreamIdx) { av_packet_unref(pkt); continue; }
        ret = avcodec_send_packet(audioDecCtx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;
        while (avcodec_receive_frame(audioDecCtx, frame) >= 0) {
            ret = av_buffersrc_add_frame_flags(audioBuffersrcCtx, frame, 0);
            av_frame_unref(frame);
            if (ret < 0) continue;
            AVFrame* filteredFrame = av_frame_alloc();
            while (true) {
                ret = av_buffersink_get_frame(audioBuffersinkCtx, filteredFrame);
                if (ret < 0) break;
                filteredFrame->pts = audioPtsCounter;
                audioPtsCounter += filteredFrame->nb_samples;
                totalAudioSamples += filteredFrame->nb_samples;
                ret = avcodec_send_frame(audioEncCtx, filteredFrame);
                if (ret < 0) { av_frame_unref(filteredFrame); continue; }
                while (ret >= 0) {
                    ret = avcodec_receive_packet(audioEncCtx, pkt);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    pkt->stream_index = audioOutStream->index;
                    av_interleaved_write_frame(ofmtCtx, pkt);
                }
                av_frame_unref(filteredFrame);
            }
            av_frame_free(&filteredFrame);
        }
        double currentTime = (av_gettime() - startTime) / 1000000.0;
        if (progressCallback && (currentTime - lastProgressTime) >= progressInterval) {
            TranscodeProgress prog = {};
            prog.time = currentTime;
            prog.percent = totalDuration > 0 ? (currentTime / totalDuration) * 100.0 : 0.0;
            prog.speed = totalDuration > 0 ? totalDuration / currentTime : 0.0;
            prog.audioFrames = totalAudioSamples;
            prog.audioTime = (totalDuration > 0 && currentTime > 0) ? currentTime * (totalDuration / currentTime) : 0.0;
            prog.estimatedDuration = totalDuration;
            prog.eta = (prog.speed > 0) ? (totalDuration - prog.audioTime) / prog.speed : 0.0;
            progressCallback(prog);
            lastProgressTime = currentTime;
        }
    }

    av_buffersrc_add_frame(audioBuffersrcCtx, nullptr);
    AVFrame* flushFrame = av_frame_alloc();
    while (av_buffersink_get_frame(audioBuffersinkCtx, flushFrame) >= 0) {
        flushFrame->pts = audioPtsCounter;
        audioPtsCounter += flushFrame->nb_samples;
        totalAudioSamples += flushFrame->nb_samples;
        if (avcodec_send_frame(audioEncCtx, flushFrame) >= 0) {
            while (avcodec_receive_packet(audioEncCtx, pkt) >= 0) {
                pkt->stream_index = audioOutStream->index;
                av_interleaved_write_frame(ofmtCtx, pkt);
            }
        }
        av_frame_unref(flushFrame);
    }
    av_frame_free(&flushFrame);

    avcodec_send_frame(audioEncCtx, nullptr);
    while (avcodec_receive_packet(audioEncCtx, pkt) >= 0) {
        pkt->stream_index = audioOutStream->index;
        av_interleaved_write_frame(ofmtCtx, pkt);
    }

    av_write_trailer(ofmtCtx);

    int64_t endTime = av_gettime();
    result.duration = totalDuration;
    result.audioFrames = totalAudioSamples;
    result.size = ofmtCtx->pb ? avio_size(ofmtCtx->pb) : 0;
    result.bitrate = result.size * 8 / (totalDuration > 0 ? totalDuration : 1);
    result.speed = (endTime - startTime) / 1000000.0 / (totalDuration > 0 ? totalDuration : 1);
    result.timeMs = (endTime - startTime) / 1000;
    success = true;

cleanup:
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (audioDecCtx) avcodec_free_context(&audioDecCtx);
    if (audioEncCtx) avcodec_free_context(&audioEncCtx);
    if (audioFilterGraph) avfilter_graph_free(&audioFilterGraph);
    if (ofmtCtx && !(ofmtCtx->oformat->flags & AVFMT_NOFILE)) avio_closep(&ofmtCtx->pb);
    if (ofmtCtx) avformat_free_context(ofmtCtx);
    if (ifmtCtx) avformat_close_input(&ifmtCtx);
    return success;
}
