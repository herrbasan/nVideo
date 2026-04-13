#include <napi.h>
#include "processor.h"
#include <sstream>
#include <map>
#include <mutex>

// Map to store audio input instances (ID -> FFmpegProcessor*)
static std::map<int, FFmpegProcessor*> g_audioInputs;
static int g_nextAudioInputId = 1;
static std::mutex g_audioInputsMutex;

static Napi::String GetVersion(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    return Napi::String::New(env, FFmpegProcessor::getVersion());
}

static Napi::Value Probe(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected string argument: path").ThrowAsJavaScriptException();
        return env.Null();
    }
    
    std::string path = info[0].As<Napi::String>().Utf8Value();
    ProbeResult result = FFmpegProcessor::probe(path.c_str());
    
    // Check if probe failed (empty result indicates failure)
    if (result.formatName.empty() && result.streams.empty()) {
        Napi::Error::New(env, "Failed to probe file: " + path).ThrowAsJavaScriptException();
        return env.Null();
    }
    
    // Create result object
    Napi::Object obj = Napi::Object::New(env);
    
    // Format info
    Napi::Object formatObj = Napi::Object::New(env);
    formatObj.Set(Napi::String::New(env, "name"), Napi::String::New(env, result.formatName));
    formatObj.Set(Napi::String::New(env, "longName"), Napi::String::New(env, result.formatLong));
    formatObj.Set(Napi::String::New(env, "duration"), Napi::Number::New(env, static_cast<double>(result.duration) / 1000000.0)); // Convert to seconds
    formatObj.Set(Napi::String::New(env, "size"), Napi::Number::New(env, static_cast<double>(result.size)));
    formatObj.Set(Napi::String::New(env, "bitrate"), Napi::Number::New(env, result.bitrate));
    obj.Set(Napi::String::New(env, "format"), formatObj);
    
    // Tags
    Napi::Object tagsObj = Napi::Object::New(env);
    for (const auto& pair : result.tags) {
        tagsObj.Set(Napi::String::New(env, pair.first), Napi::String::New(env, pair.second));
    }
    obj.Set(Napi::String::New(env, "tags"), tagsObj);
    
    // Streams
    Napi::Array streamsArray = Napi::Array::New(env, result.streams.size());
    for (size_t i = 0; i < result.streams.size(); i++) {
        const StreamInfo& si = result.streams[i];
        Napi::Object streamObj = Napi::Object::New(env);
        
        streamObj.Set(Napi::String::New(env, "index"), Napi::Number::New(env, si.index));
        streamObj.Set(Napi::String::New(env, "type"), Napi::String::New(env, si.type));
        streamObj.Set(Napi::String::New(env, "codec"), Napi::String::New(env, si.codec));
        streamObj.Set(Napi::String::New(env, "codecLong"), Napi::String::New(env, si.codecLong));
        streamObj.Set(Napi::String::New(env, "bitrate"), Napi::Number::New(env, si.bitrate));
        
        // Video-specific
        if (si.type == "video") {
            streamObj.Set(Napi::String::New(env, "width"), Napi::Number::New(env, si.width));
            streamObj.Set(Napi::String::New(env, "height"), Napi::Number::New(env, si.height));
            streamObj.Set(Napi::String::New(env, "fps"), Napi::Number::New(env, si.fps));
            streamObj.Set(Napi::String::New(env, "pixelFormat"), Napi::String::New(env, si.pixelFormat));
        }
        
        // Audio-specific
        if (si.type == "audio") {
            streamObj.Set(Napi::String::New(env, "sampleRate"), Napi::Number::New(env, si.sampleRate));
            streamObj.Set(Napi::String::New(env, "channels"), Napi::Number::New(env, si.channels));
            streamObj.Set(Napi::String::New(env, "channelLayout"), Napi::String::New(env, si.channelLayout));
            streamObj.Set(Napi::String::New(env, "bitsPerSample"), Napi::Number::New(env, si.bitsPerSample));
        }
        
        // Stream tags
        Napi::Object streamTagsObj = Napi::Object::New(env);
        for (const auto& pair : si.tags) {
            streamTagsObj.Set(Napi::String::New(env, pair.first), Napi::String::New(env, pair.second));
        }
        streamObj.Set(Napi::String::New(env, "tags"), streamTagsObj);
        
        streamsArray.Set(static_cast<uint32_t>(i), streamObj);
    }
    obj.Set(Napi::String::New(env, "streams"), streamsArray);
    
    return obj;
}

static Napi::Value GetMetadata(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected string argument: path").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::string path = info[0].As<Napi::String>().Utf8Value();
    std::map<std::string, std::string> metadata = FFmpegProcessor::getFileMetadata(path.c_str());

    Napi::Object result = Napi::Object::New(env);
    for (const auto& pair : metadata) {
        result.Set(Napi::String::New(env, pair.first), Napi::String::New(env, pair.second));
    }

    return result;
}

static Napi::Value Thumbnail(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsObject()) {
        Napi::TypeError::New(env, "Expected (string, object) arguments: path, opts").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::string path = info[0].As<Napi::String>().Utf8Value();
    Napi::Object opts = info[1].As<Napi::Object>();

    double timestamp = 0.0;
    int targetWidth = 320;

    if (opts.Has("timestamp")) {
        timestamp = opts.Get("timestamp").As<Napi::Number>().DoubleValue();
    }
    if (opts.Has("width")) {
        targetWidth = opts.Get("width").As<Napi::Number>().Int32Value();
    }

    ThumbnailResult thumbResult = FFmpegProcessor::thumbnail(path.c_str(), timestamp, targetWidth);

    if (!thumbResult.data) {
        Napi::Error::New(env, "Failed to extract thumbnail from: " + path).ThrowAsJavaScriptException();
        return env.Null();
    }

    // Create Uint8Array and copy data (zero-copy would require caller to provide buffer)
    int bufferSize = thumbResult.width * thumbResult.height * 3;
    Napi::Uint8Array data = Napi::Uint8Array::New(env, bufferSize);
    memcpy(data.Data(), thumbResult.data, bufferSize);

    Napi::Object result = Napi::Object::New(env);
    result.Set(Napi::String::New(env, "data"), data);
    result.Set(Napi::String::New(env, "width"), Napi::Number::New(env, thumbResult.width));
    result.Set(Napi::String::New(env, "height"), Napi::Number::New(env, thumbResult.height));
    result.Set(Napi::String::New(env, "pts"), Napi::Number::New(env, thumbResult.pts));
    result.Set(Napi::String::New(env, "keyframe"), Napi::Boolean::New(env, thumbResult.keyframe));
    result.Set(Napi::String::New(env, "format"), Napi::String::New(env, "rgb24"));

    return result;
}

// ==================== Audio Input Bindings ====================

struct AudioInputImpl {
    FFmpegProcessor* processor;
    int id;
};

static Napi::Value OpenInput(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected string argument: path").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::string path = info[0].As<Napi::String>().Utf8Value();

    int targetSampleRate = 44100;
    int threads = 0;

    if (info.Length() >= 2 && info[1].IsObject()) {
        Napi::Object opts = info[1].As<Napi::Object>();
        if (opts.Has("sampleRate")) {
            targetSampleRate = opts.Get("sampleRate").As<Napi::Number>().Int32Value();
        }
        if (opts.Has("threads")) {
            threads = opts.Get("threads").As<Napi::Number>().Int32Value();
        }
    }

    FFmpegProcessor* processor = new FFmpegProcessor();
    if (!processor->openInput(path.c_str(), targetSampleRate, threads)) {
        delete processor;
        Napi::Error::New(env, "Failed to open audio input: " + path).ThrowAsJavaScriptException();
        return env.Null();
    }

    // Assign ID and store in map
    int id;
    {
        std::lock_guard<std::mutex> lock(g_audioInputsMutex);
        id = g_nextAudioInputId++;
        g_audioInputs[id] = processor;
    }

    // Create JS object with methods
    Napi::Object obj = Napi::Object::New(env);
    AudioInputImpl* impl = new AudioInputImpl{processor, id};

    // Store the impl pointer as external
    obj.Set("__impl", Napi::External<AudioInputImpl>::New(env, impl, [](Napi::Env, AudioInputImpl* i) {
        delete i;
    }));

    // GetDuration
    obj.Set("getDuration", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::Number::New(env, impl->processor->getDuration());
    }));

    // GetSampleRate
    obj.Set("getSampleRate", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::Number::New(env, impl->processor->getSampleRate());
    }));

    // GetChannels
    obj.Set("getChannels", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::Number::New(env, impl->processor->getChannels());
    }));

    // GetTotalSamples
    obj.Set("getTotalSamples", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::Number::New(env, static_cast<double>(impl->processor->getTotalSamples()));
    }));

    // GetCodecName
    obj.Set("getCodecName", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::String::New(env, impl->processor->getCodecName());
    }));

    // GetInputSampleRate
    obj.Set("getInputSampleRate", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::Number::New(env, impl->processor->getInputSampleRate());
    }));

    // GetInputChannels
    obj.Set("getInputChannels", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::Number::New(env, impl->processor->getInputChannels());
    }));

    // Seek
    obj.Set("seek", Napi::Function::New(env, [](const Napi::CallbackInfo& info) -> Napi::Value {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsNumber()) {
            Napi::TypeError::New(env, "Expected number argument: seconds").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        double seconds = info[0].As<Napi::Number>().DoubleValue();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        bool success = impl->processor->seek(seconds);
        return Napi::Boolean::New(env, success);
    }));

    // ReadAudio(numSamples) - zero-copy via Float32Array
    obj.Set("readAudio", Napi::Function::New(env, [](const Napi::CallbackInfo& info) -> Napi::Value {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsNumber()) {
            Napi::TypeError::New(env, "Expected number argument: numSamples").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        int numSamples = info[0].As<Napi::Number>().Int32Value();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();

        // Allocate Float32Array for output (zero-copy)
        Napi::Float32Array audioData = Napi::Float32Array::New(env, numSamples);
        int samplesRead = impl->processor->readAudio(static_cast<float*>(audioData.Data()), numSamples);

        // Return object with samples read and audio data
        Napi::Object result = Napi::Object::New(env);
        result.Set(Napi::String::New(env, "samples"), Napi::Number::New(env, samplesRead));
        result.Set(Napi::String::New(env, "data"), audioData);
        return result;
    }));

    // Close
    obj.Set("close", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();

        {
            std::lock_guard<std::mutex> lock(g_audioInputsMutex);
            g_audioInputs.erase(impl->id);
        }

        impl->processor->closeInput();
        delete impl->processor;

        return env.Undefined();
    }));

    // isOpen
    obj.Set("isOpen", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::Boolean::New(env, impl->processor->isOpen());
    }));

    // getWaveform(numPoints) - blocking waveform generation
    obj.Set("getWaveform", Napi::Function::New(env, [](const Napi::CallbackInfo& info) -> Napi::Value {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsNumber()) {
            Napi::TypeError::New(env, "Expected number: numPoints").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        int numPoints = info[0].As<Napi::Number>().Int32Value();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();

        auto data = impl->processor->getWaveform(numPoints);

        Napi::Object result = Napi::Object::New(env);
        Napi::Float32Array peaksL = Napi::Float32Array::New(env, data.peaksL.size());
        std::copy(data.peaksL.begin(), data.peaksL.end(), peaksL.Data());
        Napi::Float32Array peaksR = Napi::Float32Array::New(env, data.peaksR.size());
        std::copy(data.peaksR.begin(), data.peaksR.end(), peaksR.Data());
        result.Set(Napi::String::New(env, "peaksL"), peaksL);
        result.Set(Napi::String::New(env, "peaksR"), peaksR);
        result.Set(Napi::String::New(env, "points"), Napi::Number::New(env, data.points));
        result.Set(Napi::String::New(env, "duration"), Napi::Number::New(env, data.duration));
        return result;
    }));

    // getWaveformStreaming(numPoints, chunkSizeMB, callback) - streaming with progress
    obj.Set("getWaveformStreaming", Napi::Function::New(env, [](const Napi::CallbackInfo& info) -> Napi::Value {
        Napi::Env env = info.Env();
        if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsFunction()) {
            Napi::TypeError::New(env, "Expected (numPoints: number, chunkSizeMB: number, callback: function)").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        int numPoints = info[0].As<Napi::Number>().Int32Value();
        int chunkSizeMB = info[1].As<Napi::Number>().Int32Value();
        Napi::Function jsCallback = info[2].As<Napi::Function>();
        int64_t chunkSizeBytes = (int64_t)chunkSizeMB * 1024 * 1024;

        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();

        // Create C++ callback that invokes JS callback
        auto callback = [&](const WaveformResult& data, float progress) -> bool {
            Napi::Object result = Napi::Object::New(env);
            Napi::Float32Array peaksL = Napi::Float32Array::New(env, data.peaksL.size());
            std::copy(data.peaksL.begin(), data.peaksL.end(), peaksL.Data());
            Napi::Float32Array peaksR = Napi::Float32Array::New(env, data.peaksR.size());
            std::copy(data.peaksR.begin(), data.peaksR.end(), peaksR.Data());
            result.Set(Napi::String::New(env, "peaksL"), peaksL);
            result.Set(Napi::String::New(env, "peaksR"), peaksR);
            result.Set(Napi::String::New(env, "points"), Napi::Number::New(env, data.points));
            result.Set(Napi::String::New(env, "duration"), Napi::Number::New(env, data.duration));
            result.Set(Napi::String::New(env, "progress"), Napi::Number::New(env, progress));

            Napi::Value jsResult = jsCallback.Call({ result });
            if (jsResult.IsBoolean()) {
                return jsResult.As<Napi::Boolean>().Value();
            }
            return true;
        };

        auto finalData = impl->processor->getWaveformStreaming(numPoints, chunkSizeBytes, callback);

        Napi::Object result = Napi::Object::New(env);
        Napi::Float32Array peaksL = Napi::Float32Array::New(env, finalData.peaksL.size());
        std::copy(finalData.peaksL.begin(), finalData.peaksL.end(), peaksL.Data());
        Napi::Float32Array peaksR = Napi::Float32Array::New(env, finalData.peaksR.size());
        std::copy(finalData.peaksR.begin(), finalData.peaksR.end(), peaksR.Data());
        result.Set(Napi::String::New(env, "peaksL"), peaksL);
        result.Set(Napi::String::New(env, "peaksR"), peaksR);
        result.Set(Napi::String::New(env, "points"), Napi::Number::New(env, finalData.points));
        result.Set(Napi::String::New(env, "duration"), Napi::Number::New(env, finalData.duration));
        return result;
    }));

    // openVideoStream - opens the video stream for frame reading
    obj.Set("openVideoStream", Napi::Function::New(env, [](const Napi::CallbackInfo& info) -> Napi::Value {
        Napi::Env env = info.Env();
        int streamIndex = -1;
        if (info.Length() >= 1 && info[0].IsNumber()) {
            streamIndex = info[0].As<Napi::Number>().Int32Value();
        }
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        bool success = impl->processor->openVideoStream(streamIndex);
        return Napi::Boolean::New(env, success);
    }));

    // getVideoWidth
    obj.Set("getVideoWidth", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::Number::New(env, impl->processor->getVideoWidth());
    }));

    // getVideoHeight
    obj.Set("getVideoHeight", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::Number::New(env, impl->processor->getVideoHeight());
    }));

    // getVideoFPS
    obj.Set("getVideoFPS", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::Number::New(env, impl->processor->getVideoFPS());
    }));

    // getVideoCodecName
    obj.Set("getVideoCodecName", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::String::New(env, impl->processor->getVideoCodecName());
    }));

    // getPosition
    obj.Set("getPosition", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        return Napi::Number::New(env, impl->processor->getPosition());
    }));

    // readVideoFrame - zero-copy into caller's buffer
    obj.Set("readVideoFrame", Napi::Function::New(env, [](const Napi::CallbackInfo& info) -> Napi::Value {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsObject()) {
            Napi::TypeError::New(env, "Expected buffer argument").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        Napi::Object bufObj = info[0].As<Napi::Object>();
        uint8_t* data = nullptr;
        size_t bufSize = 0;

        if (bufObj.IsTypedArray()) {
            Napi::TypedArray arr = bufObj.As<Napi::TypedArray>();
            if (arr.TypedArrayType() == napi_typedarray_type::napi_uint8_array ||
                arr.TypedArrayType() == napi_typedarray_type::napi_uint8_clamped_array) {
                Napi::Uint8Array uintArr = arr.As<Napi::Uint8Array>();
                data = uintArr.Data();
                bufSize = uintArr.ByteLength();
            } else {
                Napi::TypeError::New(env, "Expected Uint8Array").ThrowAsJavaScriptException();
                return env.Undefined();
            }
        } else if (bufObj.IsArrayBuffer()) {
            Napi::ArrayBuffer ab = bufObj.As<Napi::ArrayBuffer>();
            data = static_cast<uint8_t*>(ab.Data());
            bufSize = ab.ByteLength();
        } else {
            Napi::TypeError::New(env, "Expected Uint8Array or ArrayBuffer").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        int targetWidth = 0, targetHeight = 0;
        std::string targetFormat = "rgb24";

        if (info.Length() >= 2 && info[1].IsObject()) {
            Napi::Object opts = info[1].As<Napi::Object>();
            if (opts.Has("width")) targetWidth = opts.Get("width").As<Napi::Number>().Int32Value();
            if (opts.Has("height")) targetHeight = opts.Get("height").As<Napi::Number>().Int32Value();
            if (opts.Has("format")) targetFormat = opts.Get("format").As<Napi::String>().Utf8Value();
        }

        AudioInputImpl* impl = info.This().As<Napi::Object>().Get("__impl").As<Napi::External<AudioInputImpl>>().Data();
        auto result = impl->processor->readVideoFrame(data, static_cast<int>(bufSize), targetWidth, targetHeight, targetFormat.c_str());

        Napi::Object frameResult = Napi::Object::New(env);
        frameResult.Set(Napi::String::New(env, "width"), Napi::Number::New(env, result.width));
        frameResult.Set(Napi::String::New(env, "height"), Napi::Number::New(env, result.height));
        frameResult.Set(Napi::String::New(env, "pts"), Napi::Number::New(env, result.pts));
        frameResult.Set(Napi::String::New(env, "duration"), Napi::Number::New(env, result.duration));
        frameResult.Set(Napi::String::New(env, "frameNum"), Napi::Number::New(env, static_cast<double>(result.frameNum)));
        frameResult.Set(Napi::String::New(env, "keyframe"), Napi::Boolean::New(env, result.keyframe));
        frameResult.Set(Napi::String::New(env, "format"), Napi::String::New(env, result.format));

        return frameResult;
    }));

    return obj;
}

// Helper to parse TranscodeOptions from JS object
static bool ParseTranscodeOptions(Napi::Env env, Napi::Object opts, TranscodeOptions& outOpts) {
    if (opts.Has("video")) {
        Napi::Value videoVal = opts.Get("video");
        if (videoVal.IsObject()) {
            Napi::Object video = videoVal.As<Napi::Object>();
            if (video.Has("codec")) {
                outOpts.video.codec = video.Get("codec").As<Napi::String>().Utf8Value();
            }
            if (video.Has("width")) {
                outOpts.video.width = video.Get("width").As<Napi::Number>().Int32Value();
            }
            if (video.Has("height")) {
                outOpts.video.height = video.Get("height").As<Napi::Number>().Int32Value();
            }
            if (video.Has("crf")) {
                outOpts.video.crf = video.Get("crf").As<Napi::Number>().Int32Value();
            }
            if (video.Has("preset")) {
                outOpts.video.preset = video.Get("preset").As<Napi::String>().Utf8Value();
            }
            if (video.Has("pixelFormat")) {
                outOpts.video.pixelFormat = video.Get("pixelFormat").As<Napi::String>().Utf8Value();
            }
            if (video.Has("bitrate")) {
                outOpts.video.bitrate = video.Get("bitrate").As<Napi::Number>().Int64Value();
            }
            if (video.Has("fps")) {
                outOpts.video.fps = video.Get("fps").As<Napi::Number>().Int32Value();
            }
            if (video.Has("filters")) {
                outOpts.video.filters = video.Get("filters").As<Napi::String>().Utf8Value();
            }
        } else if (videoVal.IsNull() || videoVal.IsUndefined()) {
            // Video disabled
            outOpts.video.codec = "";
        }
    }

    if (opts.Has("audio")) {
        Napi::Value audioVal = opts.Get("audio");
        if (audioVal.IsObject()) {
            Napi::Object audio = audioVal.As<Napi::Object>();
            if (audio.Has("codec")) {
                outOpts.audio.codec = audio.Get("codec").As<Napi::String>().Utf8Value();
            }
            if (audio.Has("bitrate")) {
                outOpts.audio.bitrate = audio.Get("bitrate").As<Napi::Number>().Int64Value();
            }
            if (audio.Has("sampleRate")) {
                outOpts.audio.sampleRate = audio.Get("sampleRate").As<Napi::Number>().Int32Value();
            }
            if (audio.Has("channels")) {
                outOpts.audio.channels = audio.Get("channels").As<Napi::Number>().Int32Value();
            }
            if (audio.Has("filters")) {
                outOpts.audio.filters = audio.Get("filters").As<Napi::String>().Utf8Value();
            }
        } else if (audioVal.IsNull() || audioVal.IsUndefined()) {
            // Audio disabled
            outOpts.audio.codec = "";
        }
    }

    if (opts.Has("threads")) {
        outOpts.threads = opts.Get("threads").As<Napi::Number>().Int32Value();
    }

    return true;
}

// Helper to create JS progress object from TranscodeProgress
static Napi::Object ProgressToJS(Napi::Env env, const TranscodeProgress& prog) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set(Napi::String::New(env, "time"), Napi::Number::New(env, prog.time));
    obj.Set(Napi::String::New(env, "percent"), Napi::Number::New(env, prog.percent));
    obj.Set(Napi::String::New(env, "speed"), Napi::Number::New(env, prog.speed));
    obj.Set(Napi::String::New(env, "bitrate"), Napi::Number::New(env, static_cast<double>(prog.bitrate)));
    obj.Set(Napi::String::New(env, "size"), Napi::Number::New(env, static_cast<double>(prog.size)));
    obj.Set(Napi::String::New(env, "frames"), Napi::Number::New(env, static_cast<double>(prog.frames)));
    obj.Set(Napi::String::New(env, "fps"), Napi::Number::New(env, prog.fps));
    obj.Set(Napi::String::New(env, "audioFrames"), Napi::Number::New(env, static_cast<double>(prog.audioFrames)));
    obj.Set(Napi::String::New(env, "audioTime"), Napi::Number::New(env, prog.audioTime));
    obj.Set(Napi::String::New(env, "estimatedDuration"), Napi::Number::New(env, prog.estimatedDuration));
    obj.Set(Napi::String::New(env, "estimatedSize"), Napi::Number::New(env, static_cast<double>(prog.estimatedSize)));
    obj.Set(Napi::String::New(env, "eta"), Napi::Number::New(env, prog.eta));
    obj.Set(Napi::String::New(env, "dupFrames"), Napi::Number::New(env, prog.dupFrames));
    obj.Set(Napi::String::New(env, "dropFrames"), Napi::Number::New(env, prog.dropFrames));
    return obj;
}

// Helper to create JS result object from TranscodeResult
static Napi::Object ResultToJS(Napi::Env env, const TranscodeResult& result) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set(Napi::String::New(env, "duration"), Napi::Number::New(env, result.duration));
    obj.Set(Napi::String::New(env, "frames"), Napi::Number::New(env, static_cast<double>(result.frames)));
    obj.Set(Napi::String::New(env, "audioFrames"), Napi::Number::New(env, static_cast<double>(result.audioFrames)));
    obj.Set(Napi::String::New(env, "size"), Napi::Number::New(env, static_cast<double>(result.size)));
    obj.Set(Napi::String::New(env, "bitrate"), Napi::Number::New(env, static_cast<double>(result.bitrate)));
    obj.Set(Napi::String::New(env, "speed"), Napi::Number::New(env, result.speed));
    obj.Set(Napi::String::New(env, "timeMs"), Napi::Number::New(env, static_cast<double>(result.timeMs)));
    obj.Set(Napi::String::New(env, "dupFrames"), Napi::Number::New(env, result.dupFrames));
    obj.Set(Napi::String::New(env, "dropFrames"), Napi::Number::New(env, result.dropFrames));
    return obj;
}

// Helper to create JS error object from TranscodeError
static Napi::Object ErrorToJS(Napi::Env env, const TranscodeError& error) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set(Napi::String::New(env, "message"), Napi::String::New(env, error.message));
    obj.Set(Napi::String::New(env, "code"), Napi::Number::New(env, error.code));
    obj.Set(Napi::String::New(env, "operation"), Napi::String::New(env, error.operation));
    obj.Set(Napi::String::New(env, "timestamp"), Napi::Number::New(env, error.timestamp));
    obj.Set(Napi::String::New(env, "frame"), Napi::Number::New(env, static_cast<double>(error.frame)));
    obj.Set(Napi::String::New(env, "stream"), Napi::Number::New(env, error.stream));
    return obj;
}

// nVideo.transcode(input, output, opts)
static Napi::Value Transcode(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsObject()) {
        Napi::TypeError::New(env, "Expected (inputPath: string, outputPath: string, opts: object)").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::string inputPath = info[0].As<Napi::String>().Utf8Value();
    std::string outputPath = info[1].As<Napi::String>().Utf8Value();
    Napi::Object opts = info[2].As<Napi::Object>();

    TranscodeOptions transcodeOpts;
    ParseTranscodeOptions(env, opts, transcodeOpts);

    // Check for callbacks
    Napi::Function onProgress;
    Napi::Function onComplete;
    Napi::Function onError;

    if (opts.Has("onProgress") && opts.Get("onProgress").IsFunction()) {
        onProgress = opts.Get("onProgress").As<Napi::Function>();
    }
    if (opts.Has("onComplete") && opts.Get("onComplete").IsFunction()) {
        onComplete = opts.Get("onComplete").As<Napi::Function>();
    }
    if (opts.Has("onError") && opts.Get("onError").IsFunction()) {
        onError = opts.Get("onError").As<Napi::Function>();
    }

    // Create progress callback if provided
    TranscodeProgressCallback progressCb;
    if (onProgress) {
        progressCb = [&](const TranscodeProgress& prog) {
            Napi::Object progObj = ProgressToJS(env, prog);
            onProgress.Call({ progObj });
        };
    }

    TranscodeResult result;
    TranscodeError error;
    bool success = FFmpegProcessor::transcode(inputPath.c_str(), outputPath.c_str(),
                                               transcodeOpts, progressCb, result, error);

    if (!success) {
        if (onError) {
            Napi::Object errObj = ErrorToJS(env, error);
            onError.Call({ errObj });
        } else {
            Napi::Error::New(env, error.message).ThrowAsJavaScriptException();
        }
        return env.Undefined();
    }

    if (onComplete) {
        Napi::Object resultObj = ResultToJS(env, result);
        onComplete.Call({ resultObj });
    }

    return ResultToJS(env, result);
}

// nVideo.remux(input, output, opts)
static Napi::Value Remux(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected (inputPath: string, outputPath: string)").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::string inputPath = info[0].As<Napi::String>().Utf8Value();
    std::string outputPath = info[1].As<Napi::String>().Utf8Value();

    Napi::Object opts;
    if (info.Length() >= 3 && info[2].IsObject()) {
        opts = info[2].As<Napi::Object>();
    } else {
        opts = Napi::Object::New(env);
    }

    // Check for callbacks
    Napi::Function onProgress;
    Napi::Function onComplete;
    Napi::Function onError;

    if (opts.Has("onProgress") && opts.Get("onProgress").IsFunction()) {
        onProgress = opts.Get("onProgress").As<Napi::Function>();
    }
    if (opts.Has("onComplete") && opts.Get("onComplete").IsFunction()) {
        onComplete = opts.Get("onComplete").As<Napi::Function>();
    }
    if (opts.Has("onError") && opts.Get("onError").IsFunction()) {
        onError = opts.Get("onError").As<Napi::Function>();
    }

    // Create progress callback if provided
    TranscodeProgressCallback progressCb;
    if (onProgress) {
        progressCb = [&](const TranscodeProgress& prog) {
            Napi::Object progObj = ProgressToJS(env, prog);
            onProgress.Call({ progObj });
        };
    }

    TranscodeResult result;
    TranscodeError error;
    bool success = FFmpegProcessor::remux(inputPath.c_str(), outputPath.c_str(),
                                          progressCb, result, error);

    if (!success) {
        if (onError) {
            Napi::Object errObj = ErrorToJS(env, error);
            onError.Call({ errObj });
        } else {
            Napi::Error::New(env, error.message).ThrowAsJavaScriptException();
        }
        return env.Undefined();
    }

    if (onComplete) {
        Napi::Object resultObj = ResultToJS(env, result);
        onComplete.Call({ resultObj });
    }

    return ResultToJS(env, result);
}

// nVideo.concat(files, output, opts) - join multiple files
static Napi::Value Concat(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsArray() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected (files: string[], output: string, opts?: object)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Array filesArray = info[0].As<Napi::Array>();
    std::vector<std::string> files;
    for (uint32_t i = 0; i < filesArray.Length(); i++) {
        files.push_back(filesArray.Get(i).As<Napi::String>().Utf8Value());
    }

    std::string outputPath = info[1].As<Napi::String>().Utf8Value();

    Napi::Object opts;
    if (info.Length() >= 3 && info[2].IsObject()) {
        opts = info[2].As<Napi::Object>();
    } else {
        opts = Napi::Object::New(env);
    }

    // Check for callbacks
    Napi::Function onProgress;
    Napi::Function onComplete;
    Napi::Function onError;

    if (opts.Has("onProgress") && opts.Get("onProgress").IsFunction()) {
        onProgress = opts.Get("onProgress").As<Napi::Function>();
    }
    if (opts.Has("onComplete") && opts.Get("onComplete").IsFunction()) {
        onComplete = opts.Get("onComplete").As<Napi::Function>();
    }
    if (opts.Has("onError") && opts.Get("onError").IsFunction()) {
        onError = opts.Get("onError").As<Napi::Function>();
    }

    TranscodeProgressCallback progressCb;
    if (onProgress) {
        progressCb = [&](const TranscodeProgress& prog) {
            Napi::Object progObj = ProgressToJS(env, prog);
            onProgress.Call({ progObj });
        };
    }

    // Build array of C strings
    std::vector<const char*> inputPaths;
    inputPaths.reserve(files.size());
    for (const auto& f : files) {
        inputPaths.push_back(f.c_str());
    }

    TranscodeResult result;
    TranscodeError error;
    bool success = FFmpegProcessor::concat(inputPaths.data(), static_cast<int>(inputPaths.size()),
                                          outputPath.c_str(), progressCb, result, error);

    if (!success) {
        if (onError) {
            Napi::Object errObj = ErrorToJS(env, error);
            onError.Call({ errObj });
        } else {
            Napi::Error::New(env, error.message).ThrowAsJavaScriptException();
        }
        return env.Undefined();
    }

    if (onComplete) {
        Napi::Object resultObj = ResultToJS(env, result);
        onComplete.Call({ resultObj });
    }

    return ResultToJS(env, result);
}

// nVideo.extractStream(input, output, streamIndex, opts) - extract single stream
static Napi::Value ExtractStream(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsNumber()) {
        Napi::TypeError::New(env, "Expected (input: string, output: string, streamIndex: number, opts?: object)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string inputPath = info[0].As<Napi::String>().Utf8Value();
    std::string outputPath = info[1].As<Napi::String>().Utf8Value();
    int streamIndex = info[2].As<Napi::Number>().Int32Value();

    Napi::Object opts;
    if (info.Length() >= 4 && info[3].IsObject()) {
        opts = info[3].As<Napi::Object>();
    } else {
        opts = Napi::Object::New(env);
    }

    // Check for callbacks
    Napi::Function onProgress;
    Napi::Function onComplete;
    Napi::Function onError;

    if (opts.Has("onProgress") && opts.Get("onProgress").IsFunction()) {
        onProgress = opts.Get("onProgress").As<Napi::Function>();
    }
    if (opts.Has("onComplete") && opts.Get("onComplete").IsFunction()) {
        onComplete = opts.Get("onComplete").As<Napi::Function>();
    }
    if (opts.Has("onError") && opts.Get("onError").IsFunction()) {
        onError = opts.Get("onError").As<Napi::Function>();
    }

    TranscodeProgressCallback progressCb;
    if (onProgress) {
        progressCb = [&](const TranscodeProgress& prog) {
            Napi::Object progObj = ProgressToJS(env, prog);
            onProgress.Call({ progObj });
        };
    }

    TranscodeResult result;
    TranscodeError error;
    bool success = FFmpegProcessor::extractStream(inputPath.c_str(), outputPath.c_str(),
                                                  streamIndex, progressCb, result, error);

    if (!success) {
        if (onError) {
            Napi::Object errObj = ErrorToJS(env, error);
            onError.Call({ errObj });
        } else {
            Napi::Error::New(env, error.message).ThrowAsJavaScriptException();
        }
        return env.Undefined();
    }

    if (onComplete) {
        Napi::Object resultObj = ResultToJS(env, result);
        onComplete.Call({ resultObj });
    }

    return ResultToJS(env, result);
}

// nVideo.extractAudio(input, output, opts) - decode audio from video, re-encode to target format
static Napi::Value ExtractAudio(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected (input: string, output: string, opts?: object)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string inputPath = info[0].As<Napi::String>().Utf8Value();
    std::string outputPath = info[1].As<Napi::String>().Utf8Value();

    Napi::Object opts;
    if (info.Length() >= 3 && info[2].IsObject()) {
        opts = info[2].As<Napi::Object>();
    } else {
        opts = Napi::Object::New(env);
    }

    Napi::Function onProgress;
    Napi::Function onComplete;
    Napi::Function onError;

    if (opts.Has("onProgress") && opts.Get("onProgress").IsFunction()) {
        onProgress = opts.Get("onProgress").As<Napi::Function>();
    }
    if (opts.Has("onComplete") && opts.Get("onComplete").IsFunction()) {
        onComplete = opts.Get("onComplete").As<Napi::Function>();
    }
    if (opts.Has("onError") && opts.Get("onError").IsFunction()) {
        onError = opts.Get("onError").As<Napi::Function>();
    }

    TranscodeOptions::AudioOpts audioOpts;
    if (opts.Has("codec")) audioOpts.codec = opts.Get("codec").As<Napi::String>().Utf8Value();
    if (opts.Has("bitrate")) audioOpts.bitrate = opts.Get("bitrate").As<Napi::Number>().Int64Value();
    if (opts.Has("sampleRate")) audioOpts.sampleRate = opts.Get("sampleRate").As<Napi::Number>().Int32Value();
    if (opts.Has("channels")) audioOpts.channels = opts.Get("channels").As<Napi::Number>().Int32Value();
    if (opts.Has("filters")) audioOpts.filters = opts.Get("filters").As<Napi::String>().Utf8Value();

    TranscodeProgressCallback progressCb;
    if (onProgress) {
        progressCb = [&](const TranscodeProgress& prog) {
            Napi::Object progObj = ProgressToJS(env, prog);
            onProgress.Call({ progObj });
        };
    }

    TranscodeResult result;
    TranscodeError error;
    bool success = FFmpegProcessor::extractAudio(inputPath.c_str(), outputPath.c_str(),
                                                  audioOpts, progressCb, result, error);

    if (!success) {
        if (onError) {
            Napi::Object errObj = ErrorToJS(env, error);
            onError.Call({ errObj });
        } else {
            Napi::Error::New(env, error.message).ThrowAsJavaScriptException();
        }
        return env.Undefined();
    }

    if (onComplete) {
        Napi::Object resultObj = ResultToJS(env, result);
        onComplete.Call({ resultObj });
    }

    return ResultToJS(env, result);
}

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set(Napi::String::New(env, "version"), Napi::Function::New(env, GetVersion));
    exports.Set(Napi::String::New(env, "probe"), Napi::Function::New(env, Probe));
    exports.Set(Napi::String::New(env, "getMetadata"), Napi::Function::New(env, GetMetadata));
    exports.Set(Napi::String::New(env, "thumbnail"), Napi::Function::New(env, Thumbnail));
    exports.Set(Napi::String::New(env, "openInput"), Napi::Function::New(env, OpenInput));
    exports.Set(Napi::String::New(env, "transcode"), Napi::Function::New(env, Transcode));
    exports.Set(Napi::String::New(env, "remux"), Napi::Function::New(env, Remux));
    exports.Set(Napi::String::New(env, "concat"), Napi::Function::New(env, Concat));
    exports.Set(Napi::String::New(env, "extractStream"), Napi::Function::New(env, ExtractStream));
    exports.Set(Napi::String::New(env, "extractAudio"), Napi::Function::New(env, ExtractAudio));
    return exports;
}

NODE_API_MODULE(nvideo, Init)
