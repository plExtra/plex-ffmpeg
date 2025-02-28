/*
 * Copyright (c) 2016 Plex, Inc.
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

#include "plex.h"
#include "ffmpeg.h"
#include "opt_common.h"

#include "config_components.h"

#include <sys/types.h>
#include <limits.h>
#include "strings.h"
#include "libavcodec/mpegvideo.h"
#include "libavfilter/vf_inlineass.h"
#include "libavformat/http.h"
#include "libavutil/bprint.h"
#include "libavutil/timestamp.h"
#include "libavformat/internal.h"
#include "libavutil/thread.h"

PlexContext plexContext = {0};

#define LOG_LINE_SIZE 1024

#if HAVE_PTHREADS
static pthread_key_t logging_key, using_http_key, cur_line_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

static void make_keys(void)
{
    pthread_key_create(&logging_key, NULL);
    pthread_key_create(&using_http_key, NULL);
    pthread_key_create(&cur_line_key, NULL);
}
#endif

static int av_log_level_plex = AV_LOG_QUIET;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int av_log_get_level_plex(void)
{
    return av_log_level_plex;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void av_log_set_level_plex(int level)
{
    av_log_level_plex = level;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
char* PMS_IssueHttpRequest(const char* url, const char* verb)
{
    char* reply = NULL;
    static AVIOContext *ioctx = NULL;
    static pthread_mutex_t req_mutex = PTHREAD_MUTEX_INITIALIZER;
    int size = 0;
    int ret = 0;
    const char *token = getenv("X_PLEX_TOKEN");
#if HAVE_PTHREADS
    pthread_once(&key_once, make_keys);
#else
    static __thread int using_http = 0;
#endif

#if HAVE_PTHREADS
    if (pthread_getspecific(using_http_key))
        return NULL;
    pthread_setspecific(using_http_key, (void*)1);
#else
    if (using_http)
        return NULL;
    using_http = 1;
#endif

    pthread_mutex_lock(&req_mutex);

    if (ioctx) {
        // Try to reuse the existing context
        if ((ret = avformat_http_do_new_request(ioctx, url, verb)) < 0)
            avio_closep(&ioctx);
    }

    if (!ioctx) {
        AVDictionary *settings = NULL;
        av_dict_set(&settings, "method", verb, 0);
        av_dict_set(&settings, "multiple_requests", "1", 0);
        if (token && *token) {
            char headers[1024];
            snprintf(headers, sizeof(headers), "X-Plex-Token: %s\r\nX-Plex-Http-Pipeline: infinite\r\n", token);
            av_dict_set(&settings, "headers", headers, 0);
        }

        ret = avio_open2(&ioctx, url, AVIO_FLAG_READ, NULL, &settings);
        av_dict_free(&settings);
        if (ret < 0)
            goto fail;
    }

    size = avio_size(ioctx);
    if (size < 0)
        size = 4095;

    reply = av_malloc(size + 1);
    if (!reply) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if ((ret = avio_read(ioctx, reply, size)) < 0)
        goto fail;

    reply[ret] = 0;

fail:
    if (ret < 0) {
        avio_closep(&ioctx);
        if (reply)
            av_freep(&reply);
    }

#if HAVE_PTHREADS
    pthread_setspecific(using_http_key, NULL);
#else
    using_http = 0;
#endif

    pthread_mutex_unlock(&req_mutex);

    return reply;
}

void PMS_Log(LogLevel level, const char* format, ...)
{
    // Format the mesage.
    char msg[2048];
    char url[4096];
    va_list va;
    AVBPrint dstbuf;
    if (av_log_level_plex == AV_LOG_QUIET)
        return;

    va_start(va, format);
    vsnprintf(msg, sizeof(msg), format, va);
    va_end(va);

    av_bprint_init_for_buffer(&dstbuf, url, sizeof(url));

    // Build the URL.
    if (plexContext.progress_url)
        av_bprintf(&dstbuf, "%s/log?", plexContext.progress_url);
    else
        av_bprintf(&dstbuf, "http://127.0.0.1:32400/log?source=Transcoder&");
    av_bprintf(&dstbuf, "level=%d&message=", level);
    av_bprint_escape(&dstbuf, msg, NULL, AV_ESCAPE_MODE_URL, 0);

    // Issue the request.
    av_free(PMS_IssueHttpRequest(url, "POST"));
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static void plex_log_callback(void* ptr, int level, const char* fmt, va_list vl)
{
    int print_prefix = 1;
    char line[1024];
    va_list vl2;

#if HAVE_PTHREADS
    char *cur_line;
    pthread_once(&key_once, make_keys);
#else
    static __thread char cur_line[LOG_LINE_SIZE] = {0};
    static __thread int logging = 0;
#endif

    va_copy(vl2, vl);
    av_log_default_callback(ptr, level, fmt, vl2);

    if (level > av_log_level_plex)
        return;

    //Avoid recusive logging
#if HAVE_PTHREADS
    if (pthread_getspecific(logging_key))
        return;
    cur_line = pthread_getspecific(cur_line_key);
    if (!cur_line) {
        cur_line = av_mallocz(LOG_LINE_SIZE);
        if (!cur_line)
            return;
        pthread_setspecific(cur_line_key, cur_line);
    }
    pthread_setspecific(logging_key, (void*)1);
#else
    if (logging)
        return;
    logging = 1;
#endif

    print_prefix = cur_line[0] == 0;

    av_log_format_line(ptr, level, fmt, vl, line, sizeof(line), &print_prefix);
    av_strlcat(cur_line, line, LOG_LINE_SIZE);
    if (print_prefix) {
        int len = strlen(cur_line);
        if (len) {
            cur_line[len - 1] = 0;
            PMS_Log(level < AV_LOG_ERROR ? LOG_LEVEL_ERROR : (level / 8) - 2, "%s", cur_line);
            cur_line[0] = 0;
        }
    }

#if HAVE_PTHREADS
    pthread_setspecific(logging_key, NULL);
#else
    logging = 0;
#endif
}

void plex_report_stream(const AVStream *st)
{
    if (plexContext.progress_url &&
        (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
         st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) &&
        !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        char url[4096];
        AVBPrint dstbuf;
        const char *profile = avcodec_profile_name(st->codecpar->codec_id, st->codecpar->profile);

        av_bprint_init_for_buffer(&dstbuf, url, sizeof(url));
        av_bprintf(&dstbuf, "%s/stream?index=%i&id=%i&codec=%s&type=%s",
                   plexContext.progress_url, st->index, st->id,
                   avcodec_get_name(st->codecpar->codec_id),
                   av_get_media_type_string(st->codecpar->codec_type));

        if (profile) {
            av_bprintf(&dstbuf, "&profile=");
            av_bprint_escape(&dstbuf, profile, NULL, AV_ESCAPE_MODE_URL, 0);
        }

        av_free(PMS_IssueHttpRequest(url, "PUT"));
    }
}

void plex_report_stream_detail(AVStream *st)
{
    FFStream *const sti = ffstream(st);

    if ((st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
         st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) &&
        sti->codec_info_nb_frames_total == 0)
        return; // Unparsed stream; will be skipped in output

    if (plexContext.progress_url &&
        (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
         st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
         st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) &&
        !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        char url[4096];
        AVBPrint dstbuf;
        AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        const char *profile = avcodec_profile_name(st->codecpar->codec_id, st->codecpar->profile);

        av_bprint_init_for_buffer(&dstbuf, url, sizeof(url));

        av_bprintf(&dstbuf, "%s/streamDetail?index=%i&id=%i&codec=%s&type=%s",
                   plexContext.progress_url, st->index, st->id,
                   avcodec_get_name(st->codecpar->codec_id),
                   av_get_media_type_string(st->codecpar->codec_type));

        if (st->codecpar->bit_rate)
            av_bprintf(&dstbuf, "&bitrate=%"PRId64, st->codecpar->bit_rate);

        if (profile) {
            av_bprintf(&dstbuf, "&profile=");
            av_bprint_escape(&dstbuf, profile, NULL, AV_ESCAPE_MODE_URL, 0);
        }

        if (lang && lang->value && *lang->value) {
            av_bprintf(&dstbuf, "&language=");
            av_bprint_escape(&dstbuf, lang->value, NULL, AV_ESCAPE_MODE_URL, 0);
        }

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            av_bprintf(&dstbuf, "&width=%i&height=%i",
                        st->codecpar->width, st->codecpar->height);
            av_bprintf(&dstbuf, "&interlaced=%i",
                        (st->codecpar->field_order != AV_FIELD_PROGRESSIVE &&
                         st->codecpar->field_order != AV_FIELD_UNKNOWN));
            if (st->codecpar->separate_fields)
                av_bprintf(&dstbuf, "&separateFields=1");
            if (st->codecpar->sample_aspect_ratio.num && st->codecpar->sample_aspect_ratio.den)
                av_bprintf(&dstbuf, "&sar=%d:%d",
                            st->codecpar->sample_aspect_ratio.num,
                            st->codecpar->sample_aspect_ratio.den);
            if (st->codecpar->level != FF_LEVEL_UNKNOWN)
                av_bprintf(&dstbuf, "&level=%d", st->codecpar->level);
            if (st->avg_frame_rate.num && st->avg_frame_rate.den)
                av_bprintf(&dstbuf, "&frameRate=%.3f",
                            av_q2d(st->avg_frame_rate));
            if (sti && sti->avctx &&
                sti->avctx->properties & FF_CODEC_PROPERTY_CLOSED_CAPTIONS)
                av_bprintf(&dstbuf, "&closedCaptions=1");
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            char layout[256];
            char *layoutP = layout;
            char *l, *r;
            av_bprintf(&dstbuf, "&channels=%i", st->codecpar->ch_layout.nb_channels);
            av_channel_layout_describe(&st->codecpar->ch_layout, layout, sizeof(layout));
            l = strchr(layout, '(');
            r = strrchr(layout, ')');
            if (l && r && l > layoutP + 8) {
                layoutP = l + 1;
                *r = 0;
            }
            av_bprintf(&dstbuf, "&layout=");
            av_bprint_escape(&dstbuf, layout, NULL, AV_ESCAPE_MODE_URL, 0);
            av_bprintf(&dstbuf, "&sampleRate=%i", st->codecpar->sample_rate);
            if (st->codecpar->bits_per_raw_sample)
                av_bprintf(&dstbuf, "&bitDepth=%i", st->codecpar->bits_per_raw_sample);
        }

#define SEND_DISPOSITION(flagname, name) if (st->disposition & AV_DISPOSITION_##flagname) \
    av_bprintf(&dstbuf, "&disp_" name "=1");

        SEND_DISPOSITION(DEFAULT,          "default");
        SEND_DISPOSITION(DUB,              "dub");
        SEND_DISPOSITION(ORIGINAL,         "original");
        SEND_DISPOSITION(COMMENT,          "comment");
        SEND_DISPOSITION(LYRICS,           "lyrics");
        SEND_DISPOSITION(KARAOKE,          "karaoke");
        SEND_DISPOSITION(FORCED,           "forced");
        SEND_DISPOSITION(HEARING_IMPAIRED, "hearing_impaired");
        SEND_DISPOSITION(VISUAL_IMPAIRED,  "visual_impaired");
        SEND_DISPOSITION(CLEAN_EFFECTS,    "clean_effects");
        SEND_DISPOSITION(ATTACHED_PIC,     "attached_pic");
        SEND_DISPOSITION(TIMED_THUMBNAILS, "timed_thumbnails");
        SEND_DISPOSITION(CAPTIONS,         "captions");
        SEND_DISPOSITION(DESCRIPTIONS,     "descriptions");
        SEND_DISPOSITION(METADATA,         "metadata");
        SEND_DISPOSITION(DEPENDENT,        "dependent");
        SEND_DISPOSITION(STILL_IMAGE,      "still_image");

        av_free(PMS_IssueHttpRequest(url, "PUT"));
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_init(int argc, char **argv, const OptionDef *options)
{
    int idx;
    av_log_set_callback(plex_log_callback);

    idx = locate_option(argc, argv, options, "loglevel_plex");
    if (idx && argv[idx + 1])
        opt_loglevel((void*)&av_log_set_level_plex, "loglevel_plex", argv[idx + 1]);
    idx = locate_option(argc, argv, options, "progressurl");
    if (idx && argv[idx + 1])
        plex_opt_progress_url(NULL, "progressurl", argv[idx + 1]);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_prepare_setup_streams_for_input_stream(InputStream* ist)
{
#if CONFIG_INLINEASS_FILTER
    int i;
    for (i = 0; i < plexContext.nb_inlineass_ctxs; i++) {
        InlineAssContext *ctx = &plexContext.inlineass_ctxs[i];
        if (ist->st->index == ctx->stream_index &&
            ist->file_index == ctx->file_index) {
            ist->discard = 0;
            ist->st->discard = AVDISCARD_NONE;
        }
    }
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_link_subtitles_to_graph(AVFilterGraph* g)
{
#if CONFIG_INLINEASS_FILTER
    int contextId = 0;
    for (int i = 0; i < nb_filtergraphs && contextId < plexContext.nb_inlineass_ctxs; i++) {
        AVFilterGraph* graph = filtergraphs[i]->graph;
        if (!graph)
            continue;
        for (int i = 0; i < graph->nb_filters && contextId < plexContext.nb_inlineass_ctxs; i++) {
            const AVFilterContext* filterCtx = graph->filters[i];
            if (strcmp(filterCtx->filter->name, "inlineass") == 0) {
                AVFilterContext *ctx = graph->filters[i];
                InlineAssContext *assCtx = &plexContext.inlineass_ctxs[contextId++];
                assCtx->ctx = ctx;
                if (assCtx->width && assCtx->height)
                    avfilter_inlineass_set_storage_size(ctx, assCtx->width, assCtx->height);

                for (int j = 0; j < nb_input_streams; j++) {
                    InputStream *ist = input_streams[j];
                    if (ist->st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT)
                        avfilter_inlineass_add_attachment(ctx, ist->st);
                    if (ist->file_index == assCtx->file_index &&
                        ist->st->index == assCtx->stream_index &&
                        ist->sub2video.sub_queue) {
                        while (av_fifo_can_read(ist->sub2video.sub_queue)) {
                            AVSubtitle tmp;
                            av_fifo_read(ist->sub2video.sub_queue, &tmp, 1);
                            plex_process_subtitles(ist, &tmp);
                            avsubtitle_free(&tmp);
                        }
                    }
                }

                avfilter_inlineass_set_fonts(ctx);
            }
        }
    }
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_opt_subtitle_stream(void *optctx, const char *opt, const char *arg)
{
#if CONFIG_INLINEASS_FILTER
    InlineAssContext *m = NULL;
    int i, file_idx;
    char *p;
    char *map = av_strdup(arg);

    file_idx = strtol(map, &p, 0);
    if (file_idx >= nb_input_files || file_idx < 0) {
        av_log(NULL, AV_LOG_FATAL, "Invalid subtitle input file index: %d.\n", file_idx);
        goto finish;
    }

    for (i = 0; i < input_files[file_idx]->nb_streams; i++) {
        if (check_stream_specifier(input_files[file_idx]->ctx, input_files[file_idx]->ctx->streams[i],
                    *p == ':' ? p + 1 : p) <= 0)
            continue;
        if (input_files[file_idx]->ctx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            av_log(NULL, AV_LOG_ERROR, "Stream '%s' is not a subtitle stream.\n", arg);
            continue;
        }
        GROW_ARRAY(plexContext.inlineass_ctxs, plexContext.nb_inlineass_ctxs);
        m = &plexContext.inlineass_ctxs[plexContext.nb_inlineass_ctxs - 1];

        m->file_index   = file_idx;
        m->stream_index = i;
        break;
    }

finish:
    if (!m)
        av_log(NULL, AV_LOG_ERROR, "Subtitle stream map '%s' matches no streams.\n", arg);

    av_freep(&map);
#endif
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_process_subtitles(const InputStream *ist, AVSubtitle *sub)
{
#if CONFIG_INLINEASS_FILTER
    int i;
    /* If we're burning subtitles, pass discarded subtitle packets of the
     * appropriate stream  to the subtitle renderer */
    for (i = 0; i < plexContext.nb_inlineass_ctxs; i++) {
        InlineAssContext *ctx = &plexContext.inlineass_ctxs[i];
        if (ist->st->index == ctx->stream_index &&
            ist->file_index == ctx->file_index) {
            if (!ctx->ctx)
                return 1;
            avfilter_inlineass_append_data(ctx->ctx, ist->dec_ctx, sub);
            return 2;
        }
    }
#endif
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_opt_progress_url(void *optctx, const char *opt, const char *arg)
{
    plexContext.progress_url = (char*)arg;
    plex_status("startup");
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_opt_loglevel(void *o, const char *opt, const char *arg)
{
    opt_loglevel((void*)&av_log_set_level_plex, opt, arg);
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_feedback(const AVFormatContext *ic)
{
    if (plexContext.progress_url) {
        char url[4096];
        double duration = -1;
        if (ic && ic->duration != AV_NOPTS_VALUE)
            duration = ic->duration / (double)AV_TIME_BASE;
        snprintf(url, sizeof(url), "%s?duration=%f", plexContext.progress_url, duration);
        av_free(PMS_IssueHttpRequest(url, "PUT"));
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_status(const char *str)
{
    if (plexContext.progress_url) {
        char url[4096];
        snprintf(url, sizeof(url), "%s?status=%s", plexContext.progress_url, str);
        av_free(PMS_IssueHttpRequest(url, "PUT"));
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_link_input_stream(const InputStream *ist)
{
#if CONFIG_INLINEASS_FILTER
    int i;
    if (ist->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        for (i = 0; i < plexContext.nb_inlineass_ctxs; i++) {
            if (plexContext.inlineass_ctxs[i].ctx)
                avfilter_inlineass_set_storage_size(plexContext.inlineass_ctxs[i].ctx, ist->st->codecpar->width, ist->st->codecpar->height);
            plexContext.inlineass_ctxs[i].width = ist->st->codecpar->width;
            plexContext.inlineass_ctxs[i].height = ist->st->codecpar->height;
        }
#endif
}
