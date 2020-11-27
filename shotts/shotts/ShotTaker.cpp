#include "pch.h"
#include "ShotTaker.h"
#include <ranges>
#include <span>
#include <filesystem>
#include <fstream>
#include <random>

extern "C" {
#include <libavutil/imgutils.h>
}

winrt::shotts::ShotTaker::ShotTaker(std::string_view filename)
{
   if (avformat_open_input(&format_ctx_, filename.data(), nullptr, nullptr) != 0)
      InitializationError("Cannot open file");

   if (avformat_find_stream_info(format_ctx_, NULL) < 0)
      InitializationError("Cannot find stream information");

   //We only care about the "best" video stream, which is usually the longest one
   AVCodec* decoder;
   video_stream_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);

   auto codec_params = format_ctx_->streams[video_stream_]->codecpar;

   if (video_stream_ == AVERROR_DECODER_NOT_FOUND) {
      auto desc = avcodec_descriptor_get(codec_params->codec_id);
      auto err = std::string("Unsupported codec: ") + desc->name;
      InitializationError(std::move(err));
   }

   if (video_stream_ == AVERROR_STREAM_NOT_FOUND)
      InitializationError("Cannot find video stream");

   InitializeDecoder(decoder, codec_params);
   InitializeEncoder(decoder_ctx_, codec_params, format_ctx_->streams[video_stream_]);
   InitializeColorSpaceConverter(decoder_ctx_);
}

void winrt::shotts::ShotTaker::InitializeDecoder(AVCodec* decoder, AVCodecParameters* codec_params)
{
   decoder_ctx_ = avcodec_alloc_context3(decoder);
   avcodec_parameters_to_context(decoder_ctx_, codec_params);

   if (avcodec_open2(decoder_ctx_, decoder, nullptr) < 0) {
      auto desc = avcodec_descriptor_get(codec_params->codec_id);
      auto err = std::string("Could not open codec: ") + desc->name;
      InitializationError(std::move(err));
   }
}

void winrt::shotts::ShotTaker::InitializeEncoder(AVCodecContext* decoder_ctx, AVCodecParameters* codec_params, AVStream* stream)
{
   auto encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
   if (encoder == nullptr)
      InitializationError("Unsupported codec: png");

   encoder_ctx_ = avcodec_alloc_context3(encoder);

   encoder_ctx_->width = decoder_ctx->width;
   encoder_ctx_->height = decoder_ctx->height;
   encoder_ctx_->pix_fmt = AV_PIX_FMT_RGB24;
   encoder_ctx_->codec_type = AVMEDIA_TYPE_VIDEO;

   //Time base is the complement of the stream frame rate
   encoder_ctx_->time_base.num = stream->r_frame_rate.den;
   encoder_ctx_->time_base.den = stream->r_frame_rate.num;

   if (avcodec_open2(encoder_ctx_, encoder, nullptr) < 0)
      InitializationError("Could not open codec: png");
}

void winrt::shotts::ShotTaker::InitializeColorSpaceConverter(AVCodecContext* decoder_ctx)
{
   //Setup an image scaler which scales to the same size
   //but changes the pixel format to RGB24 (allowed by PNGs)
   sws_ctx_ = sws_getContext(decoder_ctx->width,
      decoder_ctx->height,
      decoder_ctx->pix_fmt,
      decoder_ctx->width,
      decoder_ctx->height,
      AV_PIX_FMT_RGB24,
      SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
}

void winrt::shotts::ShotTaker::InitializationError(std::string err)
{
   Cleanup();
   throw std::runtime_error(std::move(err));
}

winrt::shotts::ShotTaker::~ShotTaker()
{
   Cleanup();
}

void winrt::shotts::ShotTaker::Cleanup()
{
   //All of these are valid to call on nullptrs
   sws_freeContext(sws_ctx_);
   avcodec_free_context(&encoder_ctx_);
   avcodec_free_context(&decoder_ctx_);
   avformat_close_input(&format_ctx_);
}

void DecodingError() {
   throw std::runtime_error("Decoding error");
}

void EncodingError() {
   throw std::runtime_error("Encoding error");
}

AVPacket* DemuxVideoPacket(AVFormatContext* fmt_ctx, int video_stream) {
   auto packet = av_packet_alloc();
   int ret;

   //Keep reading frames until we get one for the right stream
   do {
      ret = av_read_frame(fmt_ctx, packet);
   } while (ret >= 0 && packet->stream_index != video_stream);
   return packet;
}



AVFrame* DecodeFrame(AVFormatContext* fmt_ctx, AVCodecContext* dec_ctx, int video_stream) {
   auto frame = av_frame_alloc();

   int ret;
   //Keep sending packets to the decoder until it gets a full frame
   do {
      auto packet = DemuxVideoPacket(fmt_ctx, video_stream);

      if (avcodec_send_packet(dec_ctx, packet) != 0) DecodingError();
      ret = avcodec_receive_frame(dec_ctx, frame);

      av_packet_free(&packet);
      if (ret == AVERROR(EINVAL) || ret == AVERROR_EOF) DecodingError();
   } while (ret == AVERROR(EAGAIN));

   return frame;
}

AVFrame* ConvertToRGB(SwsContext* sws_ctx, AVFrame* frame) {
   //Allocate the RGB frame and fill information about it
   auto rgb_frame = av_frame_alloc();
   if (!rgb_frame) throw std::runtime_error("Could not allocate frame");
   rgb_frame->width = frame->width;
   rgb_frame->height = frame->height;
   rgb_frame->format = AV_PIX_FMT_RGB24;
   av_image_alloc(rgb_frame->data, rgb_frame->linesize, rgb_frame->width, rgb_frame->height,
      static_cast<AVPixelFormat>(rgb_frame->format), 1);

   //Convert the original frame to RGB
   sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, rgb_frame->data, rgb_frame->linesize);

   return rgb_frame;
}

AVPacket* EncodeFrame(AVFrame* frame, AVCodecContext* enc_ctx) {
   //Create the output packet
   auto out_packet = av_packet_alloc();
   av_init_packet(out_packet);

   //Encode the frame
   if (avcodec_send_frame(enc_ctx, frame) != 0) EncodingError();
   if (avcodec_receive_packet(enc_ctx, out_packet) != 0) EncodingError();

   return out_packet;
}

AVPacket* EncodeNextFrameAsPng(AVFormatContext* fmt_ctx, int video_stream,
   SwsContext* sws_ctx, AVCodecContext* dec_ctx, AVCodecContext* enc_ctx) {
   auto frame = DecodeFrame(fmt_ctx, dec_ctx, video_stream);
   auto rgb_frame = ConvertToRGB(sws_ctx, frame);

   av_frame_free(&frame);

   auto packet = EncodeFrame(rgb_frame, enc_ctx);

   av_freep(&rgb_frame->data[0]);
   av_frame_free(&rgb_frame);

   return packet;
}

winrt::Windows::Storage::StorageFile WriteToTempFile(AVPacket* packet) {
   using namespace winrt::Windows::Storage;
   auto temp_folder = ApplicationData::Current().TemporaryFolder();
   auto out = temp_folder.CreateFileAsync(L"shotts_still_tmp.png", CreationCollisionOption::GenerateUniqueName).get();
   FileIO::WriteBytesAsync(out, { packet->data, packet->data + packet->size }).get();
   return out;
}

std::vector<winrt::Windows::Storage::StorageFile> winrt::shotts::ShotTaker::TakeShots(int n)
{
   //We're going to divide the runtime of the video into n sections
   //and take a random screenshot within each section
   auto step = format_ctx_->duration / (n + 1);

   std::random_device dev;
   std::mt19937_64 rng(dev());
   std::uniform_int_distribution<std::mt19937_64::result_type> dist(0, step);

   std::vector<Windows::Storage::StorageFile> paths;
   for (auto i = 0; i < n; ++i) {
      //Calculate a random timestamp inside the ith section
      auto seek_target = (step + (dist(rng) - step / 2)) * (i + 1);

      //The video duration is in the default time base, so needs rescaled to that of the video stream
      auto rescaled_seek_target = av_rescale_q(seek_target, AVRational{ 1, AV_TIME_BASE },
         format_ctx_->streams[video_stream_]->time_base);

      //Seek to the timestamp and flush the codec buffers so there aren't any stale frames
      av_seek_frame(format_ctx_, video_stream_, rescaled_seek_target, 0);
      avcodec_flush_buffers(decoder_ctx_);

      auto packet = EncodeNextFrameAsPng(format_ctx_, video_stream_, sws_ctx_, decoder_ctx_, encoder_ctx_);
      paths.push_back(WriteToTempFile(packet));

      av_packet_free(&packet);
   }

   return paths;
}