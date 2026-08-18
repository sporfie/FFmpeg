/*
 * Copyright (c) 2026 Sporfie
 *
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

/**
 * @file
 * Still image source that keeps watching its file on disk.
 *
 * The image is decoded once at init time, then a background thread stat()s the
 * file at a fixed interval and re-decodes it whenever its modification time
 * (or size, or inode) changes. Newly decoded images are handed over to the
 * filter thread, which serves them from that point on, so the overlay can be
 * swapped without restarting the filter graph. While the file is missing, a
 * fully transparent image is served instead.
 *
 * The output is always a packed 8 bit RGB format with alpha: no colorspace
 * conversion happens here, so a reload can never change the negotiated link
 * properties, and RGB to YUV conversion is left to a regular scale filter
 * (auto-inserted if needed) which knows the colorspace of its target link.
 */

/* needed for struct stat's nanosecond resolution mtime, see img2dec.c */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE

#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#include "libavcodec/avcodec.h"

#include "libavformat/avformat.h"

#include "libswscale/swscale.h"

#include "avfilter.h"
#include "filters.h"
#include "video.h"

typedef struct ImgWatchContext {
    const AVClass *class;

    /* options */
    char *filename;
    AVRational rate;
    int64_t poll_interval;          ///< delay between two stat() calls, in microseconds

    int64_t pts;
    AVFrame *cur;                   ///< frame currently served; filter thread only
    AVFrame *raw;                   ///< image decoded by init(), consumed by config_props()
    AVRational sar;

    /* Conversion target. Written before the watcher starts, read by it
     * afterwards, so it needs no locking. */
    int target_w, target_h;
    enum AVPixelFormat target_fmt;
    enum AVColorSpace target_csp;
    enum AVColorRange target_range;

    /* Owned by whoever may run convert_image(): config_props() before the
     * watcher is started, the watcher thread afterwards. Never both. */
    struct SwsContext *sws;

    /* Watcher thread only, once it runs. */
    struct stat last_st;
    int have_last_st;
    int blank;                      ///< the file is gone and a transparent image was published
    int64_t next_poll;              ///< only used when threads are unavailable

    /* shared state */
    AVMutex mutex;
    AVCond cond;
    int have_lock;
    AVFrame *next;                  ///< decoded image waiting to be picked up
    int stop;
    int reload_now;                 ///< set by the "reload" command
#if HAVE_THREADS
    pthread_t thread;
    int thread_running;
#endif
} ImgWatchContext;

#define OFFSET(x) offsetof(ImgWatchContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption imgwatch_options[] = {
    { "filename", "set the image file to read and watch",  OFFSET(filename), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "f",        "set the image file to read and watch",  OFFSET(filename), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "rate",     "set the output frame rate",             OFFSET(rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "10" }, 0, INT_MAX, FLAGS },
    { "r",        "set the output frame rate",             OFFSET(rate), AV_OPT_TYPE_VIDEO_RATE, { .str = "10" }, 0, INT_MAX, FLAGS },
    { "poll",     "set the delay between two checks of the file modification time",
                  OFFSET(poll_interval), AV_OPT_TYPE_DURATION, { .i64 = 1000000 }, 10000, INT64_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(imgwatch);

/**
 * Decode the first video frame of @p filename.
 *
 * Everything is opened and closed here: the file is not kept open between
 * reloads, so a rename() over it is picked up like any other change.
 */
static int load_image(AVFilterContext *ctx, const char *filename, AVFrame **out)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    const AVCodec *codec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    int stream_idx, flushed = 0, ret;

    ret = avformat_open_input(&fmt, filename, NULL, NULL);
    if (ret < 0)
        return ret;
    if ((ret = avformat_find_stream_info(fmt, NULL)) < 0)
        goto end;

    stream_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream_idx < 0) {
        ret = stream_idx;
        goto end;
    }

    if (!(dec = avcodec_alloc_context3(codec))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((ret = avcodec_parameters_to_context(dec, fmt->streams[stream_idx]->codecpar)) < 0)
        goto end;
    if ((ret = avcodec_open2(dec, codec, NULL)) < 0)
        goto end;

    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    while (!flushed) {
        ret = av_read_frame(fmt, pkt);
        if (ret < 0) {
            if (ret != AVERROR_EOF)
                goto end;
            flushed = 1;
            ret = avcodec_send_packet(dec, NULL);
        } else if (pkt->stream_index != stream_idx) {
            av_packet_unref(pkt);
            continue;
        } else {
            ret = avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
        }
        if (ret < 0)
            goto end;

        ret = avcodec_receive_frame(dec, frame);
        if (ret >= 0)
            goto end;
        if (ret != AVERROR(EAGAIN))
            goto end;
    }
    /* the file was decodable but held no frame at all */
    ret = AVERROR_INVALIDDATA;

end:
    if (ret >= 0) {
        *out  = frame;
        frame = NULL;
        ret   = 0;
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return ret;
}

/**
 * Convert a decoded image to the size and format negotiated on the output
 * link. Only ever an RGB to RGB repack unless the file changed geometry,
 * in which case swscale rescales it to the configured size.
 */
static int convert_image(AVFilterContext *ctx, const AVFrame *src, AVFrame **out)
{
    ImgWatchContext *s = ctx->priv;
    AVFrame *dst;
    int ret;

    s->sws = sws_getCachedContext(s->sws,
                                  src->width, src->height, (enum AVPixelFormat)src->format,
                                  s->target_w, s->target_h, s->target_fmt,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (!s->sws)
        return AVERROR(EINVAL);

    if (!(dst = av_frame_alloc()))
        return AVERROR(ENOMEM);
    dst->width  = s->target_w;
    dst->height = s->target_h;
    dst->format = s->target_fmt;
    if ((ret = av_frame_get_buffer(dst, 0)) < 0)
        goto fail;

    ret = sws_scale(s->sws, (const uint8_t * const *)src->data, src->linesize,
                    0, src->height, dst->data, dst->linesize);
    if (ret < 0)
        goto fail;

    dst->color_primaries     = src->color_primaries;
    dst->color_trc           = src->color_trc;
    dst->colorspace          = s->target_csp;
    dst->color_range         = s->target_range;
    dst->sample_aspect_ratio = s->sar;

    *out = dst;
    return 0;

fail:
    av_frame_free(&dst);
    return ret;
}

/**
 * Build a fully transparent frame matching the negotiated output link, served
 * while the file is missing.
 *
 * The formats this filter emits are all packed 8 bit RGB with alpha, so a
 * zeroed buffer is transparent whichever of the four bytes carries the alpha.
 */
static int blank_image(AVFilterContext *ctx, AVFrame **out)
{
    ImgWatchContext *s = ctx->priv;
    AVFrame *dst;
    int ret;

    av_assert1(av_pix_fmt_count_planes(s->target_fmt) == 1);

    if (!(dst = av_frame_alloc()))
        return AVERROR(ENOMEM);
    dst->width  = s->target_w;
    dst->height = s->target_h;
    dst->format = s->target_fmt;
    if ((ret = av_frame_get_buffer(dst, 0)) < 0) {
        av_frame_free(&dst);
        return ret;
    }

    memset(dst->data[0], 0, (size_t)dst->linesize[0] * dst->height);

    dst->colorspace          = s->target_csp;
    dst->color_range         = s->target_range;
    dst->sample_aspect_ratio = s->sar;

    *out = dst;
    return 0;
}

/** Hand a frame over to the filter thread, taking ownership of it. */
static void publish(ImgWatchContext *s, AVFrame *frame)
{
    ff_mutex_lock(&s->mutex);
    av_frame_free(&s->next);            /* not picked up yet, drop it */
    s->next = frame;
    ff_mutex_unlock(&s->mutex);
}

static int64_t mtime_nsec(const struct stat *st)
{
#if HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC
    return st->st_mtim.tv_nsec;
#elif defined(__APPLE__)
    return st->st_mtimespec.tv_nsec;
#else
    return 0;
#endif
}

static int file_changed(const ImgWatchContext *s, const struct stat *st)
{
    return !s->have_last_st                               ||
           st->st_mtime != s->last_st.st_mtime            ||
           mtime_nsec(st) != mtime_nsec(&s->last_st)      ||
           st->st_size  != s->last_st.st_size             ||
           st->st_ino   != s->last_st.st_ino;
}

/**
 * stat() the file and, if it changed since the last check, decode it and hand
 * the result over to the filter thread.
 *
 * A file that cannot be stat()ed is served as a fully transparent image until
 * it comes back. Any other failure is non fatal and keeps the current image:
 * a torn read of a file being written in place is retried on its next change.
 */
static void check_and_reload(AVFilterContext *ctx, int forced)
{
    ImgWatchContext *s = ctx->priv;
    AVFrame *raw = NULL, *conv = NULL;
    struct stat st;
    int ret;

    if (stat(s->filename, &st) < 0) {
        s->have_last_st = 0;            /* reload as soon as it comes back */
        if (s->blank)
            return;
        av_log(ctx, AV_LOG_WARNING, "Cannot stat '%s' (%s), showing a "
               "transparent image until it comes back\n", s->filename,
               av_err2str(AVERROR(errno)));
        if ((ret = blank_image(ctx, &conv)) < 0) {
            av_log(ctx, AV_LOG_WARNING, "Cannot build a transparent image "
                   "(%s), keeping the current one\n", av_err2str(ret));
            return;
        }
        s->blank = 1;
        publish(s, conv);
        return;
    }
    if (!forced && !file_changed(s, &st))
        return;

    /* Remember what we saw even if decoding fails: a half-written file is
     * retried on its next change rather than on every single poll. */
    s->last_st      = st;
    s->have_last_st = 1;

    if ((ret = load_image(ctx, s->filename, &raw)) < 0) {
        av_log(ctx, AV_LOG_WARNING, "Cannot reload '%s' (%s), keeping the "
               "current image\n", s->filename, av_err2str(ret));
        return;
    }

    if (raw->width != s->target_w || raw->height != s->target_h)
        av_log(ctx, AV_LOG_WARNING, "'%s' is now %dx%d, scaling it back to the "
               "%dx%d negotiated when the graph was configured\n", s->filename,
               raw->width, raw->height, s->target_w, s->target_h);

    ret = convert_image(ctx, raw, &conv);
    av_frame_free(&raw);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "Cannot convert '%s' (%s), keeping the "
               "current image\n", s->filename, av_err2str(ret));
        return;
    }

    s->blank = 0;
    publish(s, conv);

    av_log(ctx, AV_LOG_VERBOSE, "Reloaded '%s'\n", s->filename);
}

#if HAVE_THREADS
static void *watch_thread(void *arg)
{
    AVFilterContext *ctx = arg;
    ImgWatchContext *s = ctx->priv;

    ff_mutex_lock(&s->mutex);
    while (!s->stop) {
        int forced = s->reload_now;

        s->reload_now = 0;
        ff_mutex_unlock(&s->mutex);

        check_and_reload(ctx, forced);

        ff_mutex_lock(&s->mutex);
        if (!s->stop && !s->reload_now) {
            int64_t t = av_gettime() + s->poll_interval;
            struct timespec ts = { .tv_sec  =  t / 1000000,
                                   .tv_nsec = (t % 1000000) * 1000 };

            ff_cond_timedwait(&s->cond, &s->mutex, &ts);
        }
    }
    ff_mutex_unlock(&s->mutex);

    return NULL;
}
#endif

static void stop_watcher(AVFilterContext *ctx)
{
#if HAVE_THREADS
    ImgWatchContext *s = ctx->priv;

    if (!s->thread_running)
        return;

    ff_mutex_lock(&s->mutex);
    s->stop = 1;
    ff_cond_signal(&s->cond);
    ff_mutex_unlock(&s->mutex);

    pthread_join(s->thread, NULL);
    s->thread_running = 0;
    s->stop           = 0;
#endif
}

static int start_watcher(AVFilterContext *ctx)
{
#if HAVE_THREADS
    ImgWatchContext *s = ctx->priv;
    int err;

    av_assert0(!s->thread_running);
    if ((err = pthread_create(&s->thread, NULL, watch_thread, ctx))) {
        av_log(ctx, AV_LOG_ERROR, "Cannot start the watcher thread: %s\n",
               av_err2str(AVERROR(err)));
        return AVERROR(err);
    }
    s->thread_running = 1;
#else
    ImgWatchContext *s = ctx->priv;

    s->next_poll = av_gettime() + s->poll_interval;
    av_log(ctx, AV_LOG_WARNING, "Built without thread support, '%s' will be "
           "checked from the filter thread\n", s->filename);
#endif
    return 0;
}

static av_cold int imgwatch_init(AVFilterContext *ctx)
{
    ImgWatchContext *s = ctx->priv;
    int err, ret;

    if ((err = ff_mutex_init(&s->mutex, NULL)))
        return AVERROR(err);
    if ((err = ff_cond_init(&s->cond, NULL))) {
        ff_mutex_destroy(&s->mutex);
        return AVERROR(err);
    }
    s->have_lock = 1;

    if (!s->filename || !*s->filename) {
        av_log(ctx, AV_LOG_ERROR, "No filename provided\n");
        return AVERROR(EINVAL);
    }
    if (s->rate.num <= 0 || s->rate.den <= 0) {
        av_log(ctx, AV_LOG_ERROR, "Invalid frame rate %d/%d\n",
               s->rate.num, s->rate.den);
        return AVERROR(EINVAL);
    }

    /* Decoded here so that a bad path or an undecodable image is reported
     * while the graph is being built, and so that config_props() knows the
     * size of the output link. */
    if ((ret = load_image(ctx, s->filename, &s->raw)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Cannot load '%s': %s\n", s->filename,
               av_err2str(ret));
        return ret;
    }

    av_log(ctx, AV_LOG_VERBOSE, "filename:%s %dx%d %s rate:%d/%d poll:%"PRId64"ms\n",
           s->filename, s->raw->width, s->raw->height,
           av_get_pix_fmt_name(s->raw->format), s->rate.num, s->rate.den,
           s->poll_interval / 1000);

    return 0;
}

static av_cold void imgwatch_uninit(AVFilterContext *ctx)
{
    ImgWatchContext *s = ctx->priv;

    stop_watcher(ctx);

    av_frame_free(&s->cur);
    av_frame_free(&s->next);
    av_frame_free(&s->raw);
    sws_freeContext(s->sws);
    s->sws = NULL;

    if (s->have_lock) {
        ff_cond_destroy(&s->cond);
        ff_mutex_destroy(&s->mutex);
        s->have_lock = 0;
    }
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    FilterLink *l = ff_filter_link(outlink);
    ImgWatchContext *s = ctx->priv;
    struct stat st;
    int ret;

    /* The graph may be reconfigured, in which case the watcher is already
     * running against the previous target format. */
    stop_watcher(ctx);

    if (!s->raw && (ret = load_image(ctx, s->filename, &s->raw)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Cannot load '%s': %s\n", s->filename,
               av_err2str(ret));
        return ret;
    }

    s->sar = s->raw->sample_aspect_ratio.num ? s->raw->sample_aspect_ratio
                                             : (AVRational){ 1, 1 };

    outlink->w                   = s->raw->width;
    outlink->h                   = s->raw->height;
    outlink->sample_aspect_ratio = s->sar;
    outlink->time_base           = av_inv_q(s->rate);
    l->frame_rate                = s->rate;

    s->target_w     = outlink->w;
    s->target_h     = outlink->h;
    s->target_fmt   = outlink->format;
    s->target_csp   = outlink->colorspace;
    s->target_range = outlink->color_range;

    av_frame_free(&s->cur);
    ret = convert_image(ctx, s->raw, &s->cur);
    av_frame_free(&s->raw);
    if (ret < 0)
        return ret;

    s->blank        = 0;
    s->have_last_st = 0;
    if (!stat(s->filename, &st)) {
        s->last_st      = st;
        s->have_last_st = 1;
    }

    return start_watcher(ctx);
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    ImgWatchContext *s = ctx->priv;
    AVFrame *out;

    if (!ff_outlink_frame_wanted(outlink))
        return FFERROR_NOT_READY;

#if !HAVE_THREADS
    if (av_gettime() >= s->next_poll) {
        int forced = s->reload_now;

        s->reload_now = 0;
        s->next_poll  = av_gettime() + s->poll_interval;
        check_and_reload(ctx, forced);
    }
#endif

    ff_mutex_lock(&s->mutex);
    if (s->next) {
        av_frame_free(&s->cur);
        s->cur  = s->next;
        s->next = NULL;
    }
    ff_mutex_unlock(&s->mutex);

    /* Refcounted, so this hands out a reference to the same image until the
     * watcher publishes a new one. */
    if (!(out = av_frame_clone(s->cur)))
        return AVERROR(ENOMEM);

    out->pts      = s->pts++;
    out->duration = 1;

    return ff_filter_frame(outlink, out);
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *arg,
                           char *res, int res_len, int flags)
{
    ImgWatchContext *s = ctx->priv;

    if (!strcmp(cmd, "reload")) {
        ff_mutex_lock(&s->mutex);
        s->reload_now = 1;
        ff_cond_signal(&s->cond);
        ff_mutex_unlock(&s->mutex);
        return 0;
    }

    return AVERROR(ENOSYS);
}

static const AVFilterPad imgwatch_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
    },
};

const AVFilter ff_vsrc_imgwatch = {
    .name            = "imgwatch",
    .description     = NULL_IF_CONFIG_SMALL("Read a still image, reloading it when it changes on disk."),
    .priv_size       = sizeof(ImgWatchContext),
    .priv_class      = &imgwatch_class,
    .init            = imgwatch_init,
    .uninit          = imgwatch_uninit,
    .activate        = activate,
    .process_command = process_command,
    FILTER_OUTPUTS(imgwatch_outputs),
    FILTER_PIXFMTS(AV_PIX_FMT_RGBA, AV_PIX_FMT_BGRA,
                   AV_PIX_FMT_ARGB, AV_PIX_FMT_ABGR),
};
