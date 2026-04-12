#include <napi.h>
#include "processor.h"

static Napi::String GetVersion(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    return Napi::String::New(env, FFmpegProcessor::getVersion());
}

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set(Napi::String::New(env, "version"), Napi::Function::New(env, GetVersion));
    return exports;
}

NODE_API_MODULE(nvideo, Init)
