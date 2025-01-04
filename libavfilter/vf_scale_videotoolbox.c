/*
 * Copyright (c) 2021 rcombs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <float.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_videotoolbox.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "scale_eval.h"
#include "video.h"

typedef struct VTScaleContext {
    const AVClass *class;

    enum AVPixelFormat in_fmt, out_fmt;
    const AVPixFmtDescriptor *in_desc, *out_desc;

    AVBufferRef *frames_ctx;

    int passthrough;

    VTPixelTransferSessionRef session;

    /**
     * Output sw format. AV_PIX_FMT_NONE for no conversion.
     */
    enum AVPixelFormat format;

    char *w_expr;               ///< width  expression string
    char *h_expr;               ///< height expression string

    int force_original_aspect_ratio;
    int force_divisible_by;

    int average_chroma;

    enum AVColorRange range; // Unused
    enum AVColorSpace matrix;
    enum AVColorTransferCharacteristic trc;
    enum AVColorPrimaries pri;
} VTScaleContext;

static av_cold int vtscale_init(AVFilterContext *ctx)
{
    VTScaleContext *s = ctx->priv;
    OSStatus status;
    CFStringRef matrix = NULL;
    CFStringRef pri = NULL;
    CFStringRef trc = NULL;

    if ((status = VTPixelTransferSessionCreate(NULL, &s->session))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create pixel transfer session: %i\n", status);
        return AVERROR_EXTERNAL;
    }

    if ((status = VTSessionSetProperty(s->session,
                                       kVTPixelTransferPropertyKey_DownsamplingMode,
                                       s->average_chroma ? kVTDownsamplingMode_Average
                                                         : kVTDownsamplingMode_Decimate))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to set downsampling mode: %i\n", status);
        return AVERROR_EXTERNAL;
    }

    switch (s->matrix) {
    case AVCOL_SPC_BT709:
        matrix = kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2;
        break;
    case AVCOL_SPC_SMPTE170M:
        matrix = kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4;
        break;
    case AVCOL_SPC_SMPTE240M:
        matrix = kCMFormatDescriptionYCbCrMatrix_SMPTE_240M_1995;
        break;
    case AVCOL_SPC_BT2020_NCL:
        if (__builtin_available(macOS 10.11, iOS 9, *))
            matrix = kCMFormatDescriptionYCbCrMatrix_ITU_R_2020;
        else
            matrix = CFSTR("ITU_R_2020");
        break;
    case AVCOL_SPC_UNSPECIFIED:
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Unsupported color matrix selected: %i\n", s->matrix);
        return AVERROR(EINVAL);
    }

    if (matrix &&
        (status = VTSessionSetProperty(s->session,
                                       kVTPixelTransferPropertyKey_DestinationYCbCrMatrix,
                                       matrix))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to set output YCbCr matrix: %i\n", status);
        return AVERROR_EXTERNAL;
    }

    switch (s->pri) {
    case AVCOL_PRI_BT709:
        pri = kCMFormatDescriptionColorPrimaries_ITU_R_709_2;
        break;
    case AVCOL_PRI_SMPTE240M:
        pri = kCMFormatDescriptionColorPrimaries_SMPTE_C;
        break;
    case AVCOL_PRI_SMPTE431:
        if (__builtin_available(macOS 10.11, iOS 9, *))
            pri = kCMFormatDescriptionColorPrimaries_DCI_P3;
        else
            pri = CFSTR("DCI_P3");
        break;
    case AVCOL_PRI_SMPTE432:
        if (__builtin_available(macOS 10.11, iOS 9, *))
            pri = kCMFormatDescriptionColorPrimaries_P3_D65;
        else
            pri = CFSTR("P3_D65");
        break;
    case AVCOL_PRI_BT2020:
        if (__builtin_available(macOS 10.11, iOS 9, *))
            pri = kCMFormatDescriptionColorPrimaries_ITU_R_2020;
        else
            pri = CFSTR("ITU_R_2020");
        break;
    case AVCOL_PRI_JEDEC_P22:
        pri = kCMFormatDescriptionColorPrimaries_P22;
        // Equivalent to kCMFormatDescriptionColorPrimaries_EBU_3213?
        break;
    case AVCOL_PRI_UNSPECIFIED:
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Unsupported color primaries selected: %i\n", s->trc);
        return AVERROR(EINVAL);
    }

    if (pri &&
        (status = VTSessionSetProperty(s->session,
                                       kVTPixelTransferPropertyKey_DestinationColorPrimaries,
                                       pri))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to set output color primaries: %i\n", status);
        return AVERROR_EXTERNAL;
    }

    switch (s->trc) {
    case AVCOL_TRC_BT709:
        trc = kCMFormatDescriptionTransferFunction_ITU_R_709_2;
        break;
    case AVCOL_TRC_SMPTE240M:
        trc = kCMFormatDescriptionTransferFunction_SMPTE_240M_1995;
        break;
    case AVCOL_TRC_GAMMA22:
        trc = kCMFormatDescriptionTransferFunction_UseGamma;
        break;
    case AVCOL_TRC_BT2020_10:
    case AVCOL_TRC_BT2020_12:
        if (__builtin_available(macOS 10.11, iOS 9, *))
            trc = kCMFormatDescriptionTransferFunction_ITU_R_2020;
        else
            trc = CFSTR("ITU_R_2020");
        break;
    case AVCOL_TRC_SMPTE428:
        if (__builtin_available(macOS 10.12, iOS 10, *))
            trc = kCMFormatDescriptionTransferFunction_SMPTE_ST_428_1;
        else
            trc = CFSTR("SMPTE_ST_428_1");
        break;
    case AVCOL_TRC_SMPTE2084:
        if (__builtin_available(macOS 10.13, iOS 11, *))
            trc = kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ;
        else
            trc = CFSTR("SMPTE_ST_2084_PQ");
        break;
    case AVCOL_TRC_ARIB_STD_B67:
        if (__builtin_available(macOS 10.13, iOS 11, *))
            trc = kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG;
        else
            trc = CFSTR("ITU_R_2100_HLG");
        break;
    case AVCOL_TRC_LINEAR:
        if (__builtin_available(macOS 10.14, iOS 12, tvOS 12, watchOS 5, *))
            trc = kCMFormatDescriptionTransferFunction_Linear;
        else
            trc = CFSTR("Linear");
        break;
    case AVCOL_TRC_IEC61966_2_1:
        if (__builtin_available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *))
            trc = kCMFormatDescriptionTransferFunction_sRGB;
        else
            trc = CFSTR("IEC_sRGB");
        break;
    case AVCOL_TRC_UNSPECIFIED:
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Unsupported color transfer function selected: %i\n", s->trc);
        return AVERROR(EINVAL);
    }

    if (trc &&
        (status = VTSessionSetProperty(s->session,
                                       kVTPixelTransferPropertyKey_DestinationTransferFunction,
                                       trc))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to set output transfer function: %i\n", status);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static av_cold void vtscale_uninit(AVFilterContext *ctx)
{
    VTScaleContext *s = ctx->priv;

    if (s->session) {
        VTPixelTransferSessionInvalidate(s->session);
        CFRelease(s->session);
    }

    av_buffer_unref(&s->frames_ctx);
}

static av_cold int init_hwframe_ctx(VTScaleContext *s, AVBufferRef *device_ctx, int width, int height)
{
    AVBufferRef *out_ref = NULL;
    AVHWFramesContext *out_ctx;
    int ret;

    out_ref = av_hwframe_ctx_alloc(device_ctx);
    if (!out_ref)
        return AVERROR(ENOMEM);
    out_ctx = (AVHWFramesContext*)out_ref->data;

    out_ctx->format    = AV_PIX_FMT_VIDEOTOOLBOX;
    out_ctx->sw_format = s->out_fmt;
    out_ctx->width     = width;
    out_ctx->height    = height;

    ret = av_hwframe_ctx_init(out_ref);
    if (ret < 0)
        goto fail;

    s->frames_ctx = out_ref;

    return 0;
fail:
    av_buffer_unref(&out_ref);
    return ret;
}

static int format_is_supported(enum AVPixelFormat fmt)
{
    return av_map_videotoolbox_format_from_pixfmt(fmt) != 0;
}

static av_cold int init_processing_chain(AVFilterContext *ctx, int in_width, int in_height,
                                         int out_width, int out_height)
{
    VTScaleContext *s = ctx->priv;

    AVHWFramesContext *in_frames_ctx;

    int ret;

    /* check that we have a hw context */
    if (!ctx->inputs[0]->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }
    in_frames_ctx = (AVHWFramesContext*)ctx->inputs[0]->hw_frames_ctx->data;
    s->in_fmt     = in_frames_ctx->sw_format;
    s->out_fmt    = (s->format == AV_PIX_FMT_NONE) ? s->in_fmt : s->format;

    if (!format_is_supported(s->out_fmt)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported output format: %s\n",
               av_get_pix_fmt_name(s->out_fmt));
        return AVERROR(ENOSYS);
    }

    if (s->passthrough && in_width == out_width && in_height == out_height && s->in_fmt == s->out_fmt) {
        s->frames_ctx = av_buffer_ref(ctx->inputs[0]->hw_frames_ctx);
        if (!s->frames_ctx)
            return AVERROR(ENOMEM);
    } else {
        s->passthrough = 0;

        ret = init_hwframe_ctx(s, in_frames_ctx->device_ref, out_width, out_height);
        if (ret < 0)
            return ret;
    }

    ctx->outputs[0]->hw_frames_ctx = av_buffer_ref(s->frames_ctx);
    if (!ctx->outputs[0]->hw_frames_ctx)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold int vtscale_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = outlink->src->inputs[0];
    VTScaleContext *s  = ctx->priv;
    int w, h;
    int ret;

    if ((ret = ff_scale_eval_dimensions(s,
                                        s->w_expr, s->h_expr,
                                        inlink, outlink,
                                        &w, &h)) < 0)
        goto fail;

    ff_scale_adjust_dimensions(inlink, &w, &h,
                               s->force_original_aspect_ratio, s->force_divisible_by);

    if (((int64_t)h * inlink->w) > INT_MAX  ||
        ((int64_t)w * inlink->h) > INT_MAX)
        av_log(ctx, AV_LOG_ERROR, "Rescaled value for width or height is too big.\n");

    outlink->w = w;
    outlink->h = h;

    ret = init_processing_chain(ctx, inlink->w, inlink->h, w, h);
    if (ret < 0)
        return ret;

    if (inlink->sample_aspect_ratio.num) {
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h*inlink->w,
                                                             outlink->w*inlink->h},
                                                inlink->sample_aspect_ratio);
    } else {
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    }

    av_log(ctx, AV_LOG_VERBOSE, "w:%d h:%d fmt:%s -> w:%d h:%d fmt:%s%s\n",
           inlink->w, inlink->h, av_get_pix_fmt_name(s->in_fmt),
           outlink->w, outlink->h, av_get_pix_fmt_name(s->out_fmt),
           s->passthrough ? " (passthrough)" : "");

    return 0;

fail:
    return ret;
}

static int vtscale_scale(AVFilterContext *ctx, AVFrame *out, AVFrame *in)
{
    VTScaleContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;

    if ((ret = av_hwframe_get_buffer(outlink->hw_frames_ctx, out, 0)) < 0)
        return ret;

    OSStatus status = VTPixelTransferSessionTransferImage(s->session, (CVPixelBufferRef)in->data[3], (CVPixelBufferRef)out->data[3]);
    if (status) {
        av_log(ctx, AV_LOG_ERROR, "Image transfer failed: %i\n", status);
        return AVERROR_EXTERNAL;
    }

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        return ret;

    if (s->matrix != AVCOL_SPC_UNSPECIFIED)
        out->colorspace = s->matrix;

    if (s->trc != AVCOL_TRC_UNSPECIFIED)
        out->color_trc = s->trc;

    if (s->pri != AVCOL_PRI_UNSPECIFIED)
        out->color_primaries = s->pri;

    return 0;
}

static int vtscale_filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *ctx = link->dst;
    VTScaleContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];

    AVFrame *out = NULL;
    int ret = 0;

    if (s->passthrough)
        return ff_filter_frame(outlink, in);

    out = av_frame_alloc();
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = vtscale_scale(ctx, out, in);

    av_reduce(&out->sample_aspect_ratio.num, &out->sample_aspect_ratio.den,
              (int64_t)in->sample_aspect_ratio.num * outlink->h * link->w,
              (int64_t)in->sample_aspect_ratio.den * outlink->w * link->h,
              INT_MAX);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static AVFrame *vtscale_get_video_buffer(AVFilterLink *inlink, int w, int h)
{
    VTScaleContext *s = inlink->dst->priv;

    return s->passthrough ?
        ff_null_get_video_buffer   (inlink, w, h) :
        ff_default_get_video_buffer(inlink, w, h);
}

#define OFFSET(x) offsetof(VTScaleContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)
#define ENUM(x, y, z) { x, "", 0, AV_OPT_TYPE_CONST, { .i64 = y }, INT_MIN, INT_MAX, FLAGS, z }
static const AVOption options[] = {
    { "w", "Output video width",  OFFSET(w_expr), AV_OPT_TYPE_STRING, { .str = "iw" }, .flags = FLAGS },
    { "h", "Output video height", OFFSET(h_expr), AV_OPT_TYPE_STRING, { .str = "ih" }, .flags = FLAGS },
    { "format", "Output video pixel format", OFFSET(format), AV_OPT_TYPE_PIXEL_FMT, { .i64 = AV_PIX_FMT_NONE }, INT_MIN, INT_MAX, .flags=FLAGS },
    { "passthrough", "Do not process frames at all if parameters match", OFFSET(passthrough), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { "force_original_aspect_ratio", "decrease or increase w/h if necessary to keep the original AR", OFFSET(force_original_aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 2, FLAGS, "force_oar" },
        { "disable",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0 }, 0, 0, FLAGS, "force_oar" },
        { "decrease", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1 }, 0, 0, FLAGS, "force_oar" },
        { "increase", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 2 }, 0, 0, FLAGS, "force_oar" },
    { "force_divisible_by", "enforce that the output resolution is divisible by a defined integer when force_original_aspect_ratio is used", OFFSET(force_divisible_by), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, 256, FLAGS },

    { "average_chroma", "average chroma samples (rather than decimating) when downsampling", OFFSET(average_chroma), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },

    { "primaries", "output color primaries", OFFSET(pri), AV_OPT_TYPE_INT, { .i64 = AVCOL_PRI_UNSPECIFIED }, AVCOL_PRI_RESERVED0, AVCOL_PRI_NB - 1, FLAGS, "prm" },
    { "pri", "output color primaries", OFFSET(pri), AV_OPT_TYPE_INT, { .i64 = AVCOL_PRI_UNSPECIFIED }, AVCOL_PRI_RESERVED0, AVCOL_PRI_NB - 1, FLAGS, "prm" },
    ENUM("bt709",        AVCOL_PRI_BT709,      "prm"), // kCMFormatDescriptionColorPrimaries_ITU_R_709_2
    ENUM("smpte240m",    AVCOL_PRI_SMPTE240M,  "prm"), // kCMFormatDescriptionColorPrimaries_SMPTE_C
    ENUM("smpte431",     AVCOL_PRI_SMPTE431,   "prm"), // kCMFormatDescriptionColorPrimaries_DCI_P3
    ENUM("smpte432",     AVCOL_PRI_SMPTE432,   "prm"), // kCMFormatDescriptionColorPrimaries_P3_D65
    ENUM("bt2020",       AVCOL_PRI_BT2020,     "prm"), // kCMFormatDescriptionColorPrimaries_ITU_R_2020
    ENUM("jedec-p22",    AVCOL_PRI_JEDEC_P22,  "prm"), // kCMFormatDescriptionColorPrimaries_P22
    ENUM("ebu3213",      AVCOL_PRI_EBU3213,    "prm"), // kCMFormatDescriptionColorPrimaries_EBU_3213

    { "transfer", "output transfer function", OFFSET(trc), AV_OPT_TYPE_INT, { .i64 = AVCOL_TRC_UNSPECIFIED }, AVCOL_TRC_RESERVED0, AVCOL_TRC_NB - 1, FLAGS, "trc" },
    { "trc", "output transfer function", OFFSET(trc), AV_OPT_TYPE_INT, { .i64 = AVCOL_TRC_UNSPECIFIED }, AVCOL_TRC_RESERVED0, AVCOL_TRC_NB - 1, FLAGS, "trc" },
    ENUM("bt709",        AVCOL_TRC_BT709,        "trc"), // kCMFormatDescriptionTransferFunction_ITU_R_709_2
    ENUM("smpte240m",    AVCOL_TRC_SMPTE240M,    "trc"), // kCMFormatDescriptionTransferFunction_SMPTE_240M_1995
    ENUM("gamma22",      AVCOL_TRC_GAMMA22,      "trc"), // kCMFormatDescriptionTransferFunction_UseGamma
    ENUM("bt2020",       AVCOL_TRC_BT2020_10,    "trc"), // kCMFormatDescriptionTransferFunction_ITU_R_2020
    ENUM("bt2020-10",    AVCOL_TRC_BT2020_10,    "trc"), // kCMFormatDescriptionTransferFunction_ITU_R_2020
    ENUM("bt2020-12",    AVCOL_TRC_BT2020_12,    "trc"), // kCMFormatDescriptionTransferFunction_ITU_R_2020
    ENUM("smpte428",     AVCOL_TRC_SMPTE428,     "trc"), // kCMFormatDescriptionTransferFunction_SMPTE_ST_428_1
    ENUM("smpte2084",    AVCOL_TRC_SMPTE2084,    "trc"), // kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ
    ENUM("pq",           AVCOL_TRC_SMPTE2084,    "trc"), // kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ
    ENUM("arib-std-b67", AVCOL_TRC_ARIB_STD_B67, "trc"), // kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG
    ENUM("hlg",          AVCOL_TRC_ARIB_STD_B67, "trc"), // kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG
    ENUM("linear",       AVCOL_TRC_LINEAR,       "trc"), // kCMFormatDescriptionTransferFunction_Linear
    ENUM("iec61966-2-1", AVCOL_TRC_IEC61966_2_1, "trc"), // kCMFormatDescriptionTransferFunction_sRGB
    ENUM("srgb",         AVCOL_TRC_IEC61966_2_1, "trc"), // kCMFormatDescriptionTransferFunction_sRGB

    { "matrix", "output YCbCr matrix", OFFSET(matrix), AV_OPT_TYPE_INT, { .i64 = AVCOL_SPC_UNSPECIFIED }, AVCOL_SPC_RGB, AVCOL_SPC_NB - 1, FLAGS, "csp" },
    { "csp", "output YCbCr matrix", OFFSET(matrix), AV_OPT_TYPE_INT, { .i64 = AVCOL_SPC_UNSPECIFIED }, AVCOL_SPC_RGB, AVCOL_SPC_NB - 1, FLAGS, "csp" },
    ENUM("bt709",       AVCOL_SPC_BT709,       "csp"), // kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2
    ENUM("smpte170m",   AVCOL_SPC_SMPTE170M,   "csp"), // kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4
    ENUM("smpte240m",   AVCOL_SPC_SMPTE240M,   "csp"), // kCMFormatDescriptionYCbCrMatrix_SMPTE_240M_1995
    ENUM("bt2020nc",    AVCOL_SPC_BT2020_NCL,  "csp"), // kCMFormatDescriptionYCbCrMatrix_ITU_R_2020
    ENUM("bt2020ncl",   AVCOL_SPC_BT2020_NCL,  "csp"), // kCMFormatDescriptionYCbCrMatrix_ITU_R_2020

    { NULL },
};

static const AVClass vtscale_class = {
    .class_name = "vtscale",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad vtscale_inputs[] = {
    {
        .name        = "default",
        .type        = AVMEDIA_TYPE_VIDEO,
        .filter_frame = vtscale_filter_frame,
        .get_buffer.video = vtscale_get_video_buffer,
    },
};

static const AVFilterPad vtscale_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = vtscale_config_props,
    },
};

const AVFilter ff_vf_scale_videotoolbox = {
    .name      = "scale_videotoolbox",
    .description = NULL_IF_CONFIG_SMALL("GPU accelerated video resizer and colorspace converter"),

    .init          = vtscale_init,
    .uninit        = vtscale_uninit,

    .priv_size = sizeof(VTScaleContext),
    .priv_class = &vtscale_class,

    FILTER_INPUTS(vtscale_inputs),
    FILTER_OUTPUTS(vtscale_outputs),

    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_VIDEOTOOLBOX),

    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
