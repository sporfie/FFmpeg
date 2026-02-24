/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avstring.h"
#include "libavutil/display.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavcodec/refstruct.h"

#include "bsf.h"
#include "bsf_internal.h"
#include "cbs.h"
#include "cbs_bsf.h"
#include "cbs_h264.h"
#include "cbs_sei.h"
#include "h2645data.h"
#include "sei.h"

#include <arpa/inet.h>  /* For htonl, ntohl, etc. */
#include <netinet/in.h> /* For additional network functions */

#ifndef htonll
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htonll(x) ((((uint64_t)htonl(x & 0xFFFFFFFF)) << 32) | htonl(x >> 32))
#else
#define htonll(x) (x)
#endif
#endif

#define MAX_FILTER_CONTEXTS 10
static uint8_t sporfie_abstimecode_uuid[16] = {0x73, 0x70, 0x6F, 0x72, 0x66, 0x69, 0x65, 0x5F, 0x61, 0x62, 0x73, 0x74, 0x69, 0x6D, 0x65, 0x5F};

typedef struct H264AbsTimeCodeFilterContext
{
    AVBSFContext *bsf;
    int64_t first_abs_time;
    int64_t first_pts;
} H264AbsTimeCodeFilterContext;

typedef struct H264AbsTimeCodeContext
{
    CBSBSFContext common;
    uint64_t frames;
    uint64_t offset;
    H264AbsTimeCodeFilterContext filter_ctx[MAX_FILTER_CONTEXTS];
} H264AbsTimeCodeContext;

typedef struct H264AbsTimeCodeData
{
    SEIRawUserDataUnregistered udu;
    int64_t abs_time;
} H264AbsTimeCodeData;

static H264AbsTimeCodeFilterContext *context_for_stream(AVBSFContext *bsf)
{
    H264AbsTimeCodeContext *ctx = bsf->priv_data;
    H264AbsTimeCodeFilterContext *filter_ctx = NULL;

    for (int i = 0; i < MAX_FILTER_CONTEXTS; i++)
    {
        if (ctx->filter_ctx[i].bsf != bsf)
            continue;
        filter_ctx = &ctx->filter_ctx[i];
        break;
    }

    if (filter_ctx)
        return filter_ctx;

    for (int i = 0; i < MAX_FILTER_CONTEXTS; i++)
    {
        if (ctx->filter_ctx[i].bsf)
            continue;
        filter_ctx = &ctx->filter_ctx[i];
        filter_ctx->bsf = bsf;
        return filter_ctx;
    }

    return NULL;
}

static int h264_abstimecode_update_fragment(AVBSFContext *bsf, AVPacket *pkt, CodedBitstreamFragment *au)
{
    H264AbsTimeCodeContext *ctx = bsf->priv_data;
    H264AbsTimeCodeData *tcd = NULL;
    H264AbsTimeCodeFilterContext *filter_ctx = NULL;
    AVProducerReferenceTime *prft = NULL;
    int64_t pts_us, abs_time = 0;
    int err, isKey, insert;

    if (!pkt)
        return 0;

    isKey = pkt->flags & AV_PKT_FLAG_KEY;
    insert = ctx->frames == 1 || isKey;
    if (!insert)
        return 0;

    if (pkt->pts == AV_NOPTS_VALUE)
    {
        av_log(bsf, AV_LOG_DEBUG, "Invalid PTS value ignored\n");
        return 0;
    }

    filter_ctx = context_for_stream(bsf);
    if (filter_ctx == NULL)
    {
        av_log(bsf, AV_LOG_ERROR, "Failed to allocate filter context for stream.\n");
        return AVERROR(ENOMEM);
    }

    tcd = ff_refstruct_allocz(sizeof(H264AbsTimeCodeData));
    if (!tcd)
    {
        av_log(bsf, AV_LOG_ERROR, "Failed to allocate memory for user data SEI message.\n");
        return AVERROR(ENOMEM);
    }

    memcpy(tcd->udu.uuid_iso_iec_11578, sporfie_abstimecode_uuid, 16);
    tcd->udu.data = (uint8_t *)&(tcd->abs_time);
    tcd->udu.data_length = sizeof(int64_t);

    pts_us = av_rescale_q(pkt->pts, pkt->time_base, AV_TIME_BASE_Q);

    // Look for the absolute time in the side data, the RTSP source will put it there if available.
    prft = (AVProducerReferenceTime *)av_packet_get_side_data(pkt, AV_PKT_DATA_PRFT, NULL);
    if (prft)
    {
        abs_time = prft->wallclock;
        av_log(bsf, AV_LOG_DEBUG, "From RTSP: pts %lld, epoch %lld, base: %lld, host time: %lld\n", pts_us, abs_time, abs_time - pts_us, av_gettime());
    }

    // Otherwise figure it out ourselves
    if (abs_time == 0)
    {
        int64_t rel_pts_us;
        // We need to set the frame's absolute time in the SEI message.
        // At this point, we derive it from the current time: the first received frame will have its absolute time set to the current
        // time, and the subsequent frames will have their absolute time set to that time plus the difference, in PTS,
        // between the current frame and the first frame.
        if (filter_ctx->first_abs_time == 0)
        {
            filter_ctx->first_abs_time = av_gettime() + ctx->offset;
            filter_ctx->first_pts = pkt->pts;
            av_log(bsf, AV_LOG_INFO, "First frame pts %lld, epoch %lld\n", pts_us, filter_ctx->first_abs_time);
        }
        rel_pts_us = av_rescale_q(pkt->pts - filter_ctx->first_pts, pkt->time_base, AV_TIME_BASE_Q);
        abs_time = filter_ctx->first_abs_time + rel_pts_us;
    }
    tcd->abs_time = htonll(abs_time);

    err = ff_cbs_sei_add_message(ctx->common.output, au, 1, SEI_TYPE_USER_DATA_UNREGISTERED, tcd, tcd);
    ff_refstruct_unref(&tcd);
    if (err < 0)
    {
        av_log(bsf, AV_LOG_ERROR, "Failed to add user data SEI message to access unit.\n");
        return err;
    }

    av_log(bsf, AV_LOG_TRACE, "Added user data SEI message to access unit: pts %lld, epoch %lld, base %lld\n", pts_us, abs_time, abs_time - pts_us);
    return 0;
}

static const CBSBSFType h264_abstimecode_type = {
    .codec_id = AV_CODEC_ID_H264,
    .fragment_name = "access unit",
    .unit_name = "NAL unit",
    .update_fragment = &h264_abstimecode_update_fragment,
};

static int h264_abstimecode_init(AVBSFContext *bsf)
{
    H264AbsTimeCodeContext *ctx = bsf->priv_data;
    ctx->frames = 0;
    memset(ctx->filter_ctx, 0, sizeof(ctx->filter_ctx));
    return ff_cbs_bsf_generic_init(bsf, &h264_abstimecode_type);
}

#define OFFSET(x) offsetof(H264AbsTimeCodeContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_BSF_PARAM)
static const AVOption h264_abstimecode_options[] = {
    {"frames", "Which frames to insert timecode into (0=key frames, 1=all)", OFFSET(frames), AV_OPT_TYPE_UINT64, {.i64 = 0}, 0, 1, .flags = FLAGS},
    {"offset", "Offset from actual time in microseconds", OFFSET(offset), AV_OPT_TYPE_UINT64, {.i64 = 0}, -60000000, 60000000, .flags = FLAGS},
    {NULL}};

static const AVClass h264_abstimecode_class = {
    .class_name = "h264_abstimecode_bsf",
    .item_name = av_default_item_name,
    .option = h264_abstimecode_options,
    .version = LIBAVUTIL_VERSION_INT,
};

static const enum AVCodecID h264_abstimecode_codec_ids[] = {
    AV_CODEC_ID_H264,
    AV_CODEC_ID_NONE,
};

const FFBitStreamFilter ff_h264_abstimecode_bsf = {
    .p.name = "h264_abstimecode",
    .p.codec_ids = h264_abstimecode_codec_ids,
    .p.priv_class = &h264_abstimecode_class,
    .priv_data_size = sizeof(H264AbsTimeCodeContext),
    .init = &h264_abstimecode_init,
    .close = &ff_cbs_bsf_generic_close,
    .filter = &ff_cbs_bsf_generic_filter,
};
