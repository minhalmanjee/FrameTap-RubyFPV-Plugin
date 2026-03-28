#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "../public/ruby_core_plugin.h"

using namespace std;

#define OUTPUT_DIR   "frametap_frames"
#define LOG_PATH     "frametap_frames/log.json"
#define SAVE_EVERY_N 1
#define UDP_PORT     7012

const char* g_szPluginName = "FrameTap - RubyFPV Frame Extractor";
const char* g_szPluginGUID = "FT9921-RFP44-XT01-FRAME";

static int   g_iFrameCount  = 0;
static int   g_iSavedCount  = 0;
static bool  g_bFirstIFrame = false;
static FILE* g_pLog         = NULL;
static bool  g_bFirstLog    = true;

static pthread_t g_thread;
static bool      g_bRunning = false;

void ft_mkdir(const char* path) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);
    int ret = system(cmd);
    (void)ret;
}

void ft_log(int frame_id, const char* type, int w, int h, int64_t ts) {
    if (!g_pLog) return;
    if (!g_bFirstLog) fprintf(g_pLog, ",\n");
    fprintf(g_pLog,
        "  {\n"
        "    \"frame_id\": %d,\n"
        "    \"timestamp\": %lld,\n"
        "    \"frame_type\": \"%s\",\n"
        "    \"width\": %d,\n"
        "    \"height\": %d\n"
        "  }",
        frame_id, (long long)ts, type, w, h);
    fflush(g_pLog);
    g_bFirstLog = false;
    printf("[frametap] frame %d | %s | %dx%d\n", frame_id, type, w, h);
}

void* ft_udp_thread(void*) {
    AVFormatContext* fmt_ctx   = NULL;
    AVCodecContext*  codec_ctx = NULL;
    SwsContext*      sws_ctx   = NULL;
    AVPacket*        pkt       = av_packet_alloc();
    AVFrame*         frame     = av_frame_alloc();
    AVFrame*         rgb_frame = av_frame_alloc();

    char url[64];
    snprintf(url, sizeof(url), "udp://127.0.0.1:%d", UDP_PORT);

    AVDictionary* opts = NULL;
    av_dict_set(&opts, "buffer_size", "1024000", 0);
    av_dict_set(&opts, "max_delay",   "500000",  0);

    if (avformat_open_input(&fmt_ctx, url, NULL, &opts) < 0) {
        printf("[frametap] failed to open stream %s\n", url);
        return NULL;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        printf("[frametap] failed to find stream info\n");
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    int video_stream = -1;
    for (int i = 0; i < (int)fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = i;
            break;
        }
    }

    if (video_stream < 0) {
        printf("[frametap] no video stream found\n");
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    AVCodecParameters* par   = fmt_ctx->streams[video_stream]->codecpar;
    const AVCodec*     codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        printf("[frametap] decoder not found\n");
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, par);
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        printf("[frametap] failed to open codec\n");
        avformat_close_input(&fmt_ctx);
        return NULL;
    }

    printf("[frametap] stream opened — decoding %dx%d\n",
        codec_ctx->width, codec_ctx->height);

    while (g_bRunning) {
        if (av_read_frame(fmt_ctx, pkt) < 0) break;

        if (pkt->stream_index != video_stream) {
            av_packet_unref(pkt);
            continue;
        }

        bool is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;

        if (!g_bFirstIFrame && !is_keyframe) {
            av_packet_unref(pkt);
            continue;
        }
        g_bFirstIFrame = true;

        g_iFrameCount++;
        if (g_iFrameCount % SAVE_EVERY_N != 0) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(codec_ctx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        if (avcodec_receive_frame(codec_ctx, frame) < 0) continue;

        if (!sws_ctx) {
            sws_ctx = sws_getContext(
                frame->width, frame->height, (AVPixelFormat)frame->format,
                frame->width, frame->height, AV_PIX_FMT_RGB24,
                SWS_BILINEAR, NULL, NULL, NULL);
            av_image_alloc(rgb_frame->data, rgb_frame->linesize,
                frame->width, frame->height, AV_PIX_FMT_RGB24, 1);
        }

        sws_scale(sws_ctx,
            frame->data, frame->linesize, 0, frame->height,
            rgb_frame->data, rgb_frame->linesize);

        time_t t = time(NULL);
        struct tm* tm_info = localtime(&t);
        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%Y%m%d_%H%M%S", tm_info);

        char fname[256];
        snprintf(fname, sizeof(fname), "%s/frame_%05d_%s_%s.jpg",
            OUTPUT_DIR, g_iSavedCount, timebuf,
            is_keyframe ? "I" : "P");

        stbi_write_jpg(fname,
            frame->width, frame->height, 3,
            rgb_frame->data[0], 90);

        int64_t ts = (int64_t)time(NULL) * 1000;
        ft_log(g_iSavedCount,
            is_keyframe ? "I-frame" : "P-frame",
            frame->width, frame->height, ts);

        g_iSavedCount++;
    }

    if (sws_ctx)   sws_freeContext(sws_ctx);
    if (rgb_frame) av_frame_free(&rgb_frame);
    if (frame)     av_frame_free(&frame);
    if (pkt)       av_packet_free(&pkt);
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (fmt_ctx)   avformat_close_input(&fmt_ctx);
    return NULL;
}

#ifdef __cplusplus
extern "C" {
#endif

u32 core_plugin_on_requested_capabilities() {
    return CORE_PLUGIN_CAPABILITY_VIDEO_STREAM | CORE_PLUGIN_CAPABILITY_DATA_STREAM;
}

const char* core_plugin_get_name() { return g_szPluginName; }
const char* core_plugin_get_guid() { return g_szPluginGUID; }
int  core_plugin_get_version()     { return 1; }

int core_plugin_init(u32 uRuntimeLocation, u32 uAllocatedCapabilities) {
    ft_mkdir(OUTPUT_DIR);
    g_pLog = fopen(LOG_PATH, "w");
    if (g_pLog) fprintf(g_pLog, "[\n");

    g_bRunning = true;
    pthread_create(&g_thread, NULL, ft_udp_thread, NULL);

    printf("[frametap] plugin initialized — saving every %d frames to %s\n",
        SAVE_EVERY_N, OUTPUT_DIR);
    return 0;
}

void core_plugin_uninit() {
    g_bRunning = false;
    pthread_join(g_thread, NULL);
    if (g_pLog) { fprintf(g_pLog, "\n]"); fclose(g_pLog); }
    printf("[frametap] plugin unloaded. %d frames saved.\n", g_iSavedCount);
}

void core_plugin_on_rx_data(u8* pData, int iDataLength, int iDataType, u32 uSegmentIndex) {}

u32  core_plugin_has_pending_tx_data()                                       { return 0; }
u8*  core_plugin_on_get_segment_data(u32 uSegmentIndex)                      { return NULL; }
int  core_plugin_on_get_segment_length(u32 uSegmentIndex)                    { return 0; }
int  core_plugin_on_get_segment_type(u32 uSegmentIndex)                      { return CORE_PLUGIN_TYPE_DATA_SEGMENT; }
int  core_plugin_on_get_video_stream_source_type()                           { return CORE_PLUGIN_VIDEO_STREAM_SOURCE_IP; }
int  core_plugin_on_setup_video_stream_source()                              { return 1; }
void core_plugin_on_start_video_stream_capture(int w, int h, int f)          {}
void core_plugin_on_stop_video_stream_capture()                              {}
void core_plugin_on_start_video_stream_playback(int x, int y, int w, int h) {}
void core_plugin_on_stop_video_stream_playback()                             {}
void core_plugin_on_lower_video_capture_rate(int iMaxkbps)                   {}
void core_plugin_on_higher_video_capture_rate(int iMaxkbps)                  {}
void core_plugin_on_allocated_uart(char* szUARTName)                         {}
void core_plugin_on_stop_using_uart()                                        {}

#ifdef __cplusplus
}
#endif
