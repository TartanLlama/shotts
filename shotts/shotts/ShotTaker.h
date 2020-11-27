//ShotTaker takes a video file and produces a set of random screenshots distributed throughout the video

#pragma once

#include <string_view>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace winrt::shotts {
   class ShotTaker {
   public:
      ShotTaker(std::string_view filename);
      ~ShotTaker();

      //Take n shots and save them out to temporary files
      std::vector<winrt::Windows::Storage::StorageFile> TakeShots(int n);
   private:
      void InitializeEncoder(AVCodecContext* decoder_ctx, AVCodecParameters* codec_params, AVStream* stream);
      void InitializeColorSpaceConverter(AVCodecContext* decoder_ctx);
      void InitializeDecoder(AVCodec* decoder, AVCodecParameters* codec_params);
      void Cleanup();
      void InitializationError(std::string err);

      AVFormatContext* format_ctx_ = nullptr;
      AVCodecContext* decoder_ctx_ = nullptr;
      AVCodecContext* encoder_ctx_ = nullptr;
      SwsContext* sws_ctx_ = nullptr;
      int video_stream_ = 0;
   };
}