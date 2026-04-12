#pragma once

#include <string>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavutil/avutil.h>
}

class FFmpegProcessor {
public:
    FFmpegProcessor();
    ~FFmpegProcessor();

    static std::string getVersion();

private:
};
