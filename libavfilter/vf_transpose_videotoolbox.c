/*
 * Copyright (c) 2022 rcombs
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
#include "transpose.h"
#include "video.h"

#include <AvailabilityMacros.h>

#define AVAILABILITY_CHECK() __builtin_available(macOS 13.0, iOS 16.0, *)
#define AVAILABILITY_ATTR API_AVAILABLE(macos(13.0), ios(16.0))

typedef struct AVAILABILITY_ATTR VTTransposeContext {
    const AVClass *class;

    int passthrough;         // PassthroughType, landscape passthrough mode enabled
    int dir;                 // TransposeDir

    VTPixelRotationSessionRef session;
} VTTransposeContext;

// Using sizeof(YADIFVTContext) outside of an availability check will error
// if we're targeting an older OS version, so we need to calculate the size ourselves
// (we'll statically verify it's correct in vttranspose_init behind a check)
#define TRANSPOSE_VT_CTX_SIZE (sizeof(void*) * 2 + sizeof(int) * 2)


static av_cold int do_init(AVFilterContext *ctx) AVAILABILITY_ATTR
{
    VTTransposeContext *s = ctx->priv;
    OSStatus status;

    CFStringRef rotation_val = NULL;
    CFStringRef flip_key = NULL;

    if ((status = VTPixelRotationSessionCreate(NULL, &s->session))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create pixel rotation session: %i\n", status);
        return AVERROR_EXTERNAL;
    }

    switch (s->dir) {
    case TRANSPOSE_CCLOCK_FLIP:
        rotation_val = kVTRotation_CCW90;
        flip_key     = kVTPixelRotationPropertyKey_FlipVerticalOrientation;
        break;
    case TRANSPOSE_CLOCK:
        rotation_val = kVTRotation_CW90;
        flip_key     = NULL;
        break;
    case TRANSPOSE_CCLOCK:
        rotation_val = kVTRotation_CCW90;
        flip_key     = NULL;
        break;
    case TRANSPOSE_CLOCK_FLIP:
        rotation_val = kVTRotation_CW90;
        flip_key     = kVTPixelRotationPropertyKey_FlipVerticalOrientation;
        break;
    case TRANSPOSE_REVERSAL:
        rotation_val = kVTRotation_180;
        flip_key     = NULL;
        break;
    case TRANSPOSE_HFLIP:
        rotation_val = kVTRotation_0;
        flip_key     = kVTPixelRotationPropertyKey_FlipHorizontalOrientation;
        break;
    case TRANSPOSE_VFLIP:
        rotation_val = kVTRotation_0;
        flip_key     = kVTPixelRotationPropertyKey_FlipVerticalOrientation;
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Failed to set direction to %d\n", s->dir);
        return AVERROR(EINVAL);
    }

    if (rotation_val &&
        (status = VTSessionSetProperty(s->session,
                                       kVTPixelRotationPropertyKey_Rotation,
                                       rotation_val))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to set rotation: %i\n", status);
        return AVERROR_EXTERNAL;
    }

    if (flip_key &&
        (status = VTSessionSetProperty(s->session,
                                       flip_key,
                                       kCFBooleanTrue))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to set flip: %i\n", status);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static av_cold int vttranspose_init(AVFilterContext *ctx)
{
    if (AVAILABILITY_CHECK()) {
        // Ensure we calculated YADIF_VT_CTX_SIZE correctly
        static_assert(TRANSPOSE_VT_CTX_SIZE == sizeof(VTTransposeContext), "Incorrect TRANSPOSE_VT_CTX_SIZE value!");
        return do_init(ctx);
    } else {
        av_log(ctx, AV_LOG_ERROR, "VTPixelRotationSession is not available on this OS version\n");
        return AVERROR(ENOSYS);
    }
}

static av_cold void do_uninit(AVFilterContext *ctx) AVAILABILITY_ATTR
{
    VTTransposeContext *s = ctx->priv;

    if (s->session) {
        VTPixelRotationSessionInvalidate(s->session);
        CFRelease(s->session);
    }
}

static av_cold void vttranspose_uninit(AVFilterContext *ctx)
{
    if (AVAILABILITY_CHECK()) {
        do_uninit(ctx);
    }
}

static av_cold int init_hwframe_ctx(AVFilterContext *ctx, AVBufferRef *device_ctx, int width, int height)
{
    AVBufferRef *out_ref = NULL;
    AVHWFramesContext *out_ctx;
    int ret;

    out_ref = av_hwframe_ctx_alloc(device_ctx);
    if (!out_ref)
        return AVERROR(ENOMEM);
    out_ctx = (AVHWFramesContext*)out_ref->data;

    out_ctx->format    = AV_PIX_FMT_VIDEOTOOLBOX;
    out_ctx->sw_format = ((AVHWFramesContext*)ctx->inputs[0]->hw_frames_ctx->data)->sw_format;
    out_ctx->width     = width;
    out_ctx->height    = height;

    ret = av_hwframe_ctx_init(out_ref);
    if (ret < 0)
        goto fail;

    ctx->outputs[0]->hw_frames_ctx = out_ref;

    return 0;
fail:
    av_buffer_unref(&out_ref);
    return ret;
}

static int format_is_supported(enum AVPixelFormat fmt)
{
    return av_map_videotoolbox_format_from_pixfmt(fmt) != 0;
}

static av_cold int init_processing_chain(AVFilterContext *ctx, int out_width, int out_height) AVAILABILITY_ATTR
{
    AVHWFramesContext *in_frames_ctx;

    int ret;

    /* check that we have a hw context */
    if (!ctx->inputs[0]->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }
    in_frames_ctx = (AVHWFramesContext*)ctx->inputs[0]->hw_frames_ctx->data;

    if (!format_is_supported(in_frames_ctx->sw_format)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported pixel format: %s\n",
               av_get_pix_fmt_name(in_frames_ctx->sw_format));
        return AVERROR(ENOSYS);
    }

    ret = init_hwframe_ctx(ctx, in_frames_ctx->device_ref, out_width, out_height);
    if (ret < 0)
        return ret;

    return 0;
}

static av_cold int do_config_output(AVFilterLink *outlink) AVAILABILITY_ATTR
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = outlink->src->inputs[0];
    VTTransposeContext *s  = ctx->priv;
    int ret;

    if ((inlink->w >= inlink->h && s->passthrough == TRANSPOSE_PT_TYPE_LANDSCAPE) ||
        (inlink->w <= inlink->h && s->passthrough == TRANSPOSE_PT_TYPE_PORTRAIT)) {
        outlink->hw_frames_ctx = av_buffer_ref(inlink->hw_frames_ctx);
        if (!outlink->hw_frames_ctx)
            return AVERROR(ENOMEM);
        av_log(ctx, AV_LOG_VERBOSE,
               "w:%d h:%d -> w:%d h:%d (passthrough mode)\n",
               inlink->w, inlink->h, inlink->w, inlink->h);
        return 0;
    } else {
        s->passthrough = TRANSPOSE_PT_TYPE_NONE;
    }

    if (s->dir == TRANSPOSE_CCLOCK_FLIP ||
        s->dir == TRANSPOSE_CLOCK ||
        s->dir == TRANSPOSE_CCLOCK ||
        s->dir == TRANSPOSE_CLOCK_FLIP) {
        if (inlink->sample_aspect_ratio.num) {
            outlink->sample_aspect_ratio = av_div_q((AVRational) { 1, 1 },
                                                    inlink->sample_aspect_ratio);
        } else {
            outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
        }

        outlink->w = inlink->h;
        outlink->h = inlink->w;
    }

    ret = init_processing_chain(ctx, outlink->w, outlink->h);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_VERBOSE, "w:%d h:%d -transpose-> w:%d h:%d\n",
           inlink->w, inlink->h, outlink->w, outlink->h);

    return 0;
}

static int config_output(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    if (AVAILABILITY_CHECK()) {
        return do_config_output(link);
    } else {
        av_log(ctx, AV_LOG_ERROR, "Metal is not available on this OS version\n");
        return AVERROR(ENOSYS);
    }
}

static int vttranspose_rotate(AVFilterContext *ctx, AVFrame *out, AVFrame *in) AVAILABILITY_ATTR
{
    VTTransposeContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;

    if ((ret = av_hwframe_get_buffer(outlink->hw_frames_ctx, out, 0)) < 0)
        return ret;

    OSStatus status = VTPixelRotationSessionRotateImage(s->session, (CVPixelBufferRef)in->data[3], (CVPixelBufferRef)out->data[3]);
    if (status) {
        av_log(ctx, AV_LOG_ERROR, "Image rotation failed: %i\n", status);
        return AVERROR_EXTERNAL;
    }

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        return ret;

    out->sample_aspect_ratio = ctx->outputs[0]->sample_aspect_ratio;

    return 0;
}

static int do_filter_frame(AVFilterLink *link, AVFrame *in) AVAILABILITY_ATTR
{
    AVFilterContext *ctx = link->dst;
    VTTransposeContext *s = ctx->priv;
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

    ret = vttranspose_rotate(ctx, out, in);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static int vttranspose_filter_frame(AVFilterLink *link, AVFrame *in)
{
    if (AVAILABILITY_CHECK()) {
        return do_filter_frame(link, in);
    } else {
        return AVERROR(ENOSYS);
    }
}

static AVFrame *vttranspose_get_video_buffer(AVFilterLink *inlink, int w, int h)
{
    if (AVAILABILITY_CHECK()) {
        VTTransposeContext *s = inlink->dst->priv;

        return s->passthrough ?
            ff_null_get_video_buffer   (inlink, w, h) :
            ff_default_get_video_buffer(inlink, w, h);
    } else {
        return NULL;
    }
}

#define OFFSET(x) offsetof(VTTransposeContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)
#define ENUM(x, y, z) { x, "", 0, AV_OPT_TYPE_CONST, { .i64 = y }, INT_MIN, INT_MAX, FLAGS, z }
static const AVOption options[] = {
    { "dir", "set transpose direction", OFFSET(dir), AV_OPT_TYPE_INT, { .i64 = TRANSPOSE_CCLOCK_FLIP }, 0, 6, FLAGS, "dir" },
        { "cclock_flip",   "rotate counter-clockwise with vertical flip", 0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CCLOCK_FLIP }, .flags=FLAGS, .unit = "dir" },
        { "clock",         "rotate clockwise",                            0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CLOCK       }, .flags=FLAGS, .unit = "dir" },
        { "cclock",        "rotate counter-clockwise",                    0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CCLOCK      }, .flags=FLAGS, .unit = "dir" },
        { "clock_flip",    "rotate clockwise with vertical flip",         0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_CLOCK_FLIP  }, .flags=FLAGS, .unit = "dir" },
        { "reversal",      "rotate by half-turn",                         0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_REVERSAL    }, .flags=FLAGS, .unit = "dir" },
        { "hflip",         "flip horizontally",                           0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_HFLIP       }, .flags=FLAGS, .unit = "dir" },
        { "vflip",         "flip vertically",                             0, AV_OPT_TYPE_CONST, { .i64 = TRANSPOSE_VFLIP       }, .flags=FLAGS, .unit = "dir" },

    { "passthrough", "do not apply transposition if the input matches the specified geometry",
      OFFSET(passthrough), AV_OPT_TYPE_INT, {.i64=TRANSPOSE_PT_TYPE_NONE},  0, INT_MAX, FLAGS, "passthrough" },
        { "none",      "always apply transposition",   0, AV_OPT_TYPE_CONST, {.i64=TRANSPOSE_PT_TYPE_NONE},      INT_MIN, INT_MAX, FLAGS, "passthrough" },
        { "portrait",  "preserve portrait geometry",   0, AV_OPT_TYPE_CONST, {.i64=TRANSPOSE_PT_TYPE_PORTRAIT},  INT_MIN, INT_MAX, FLAGS, "passthrough" },
        { "landscape", "preserve landscape geometry",  0, AV_OPT_TYPE_CONST, {.i64=TRANSPOSE_PT_TYPE_LANDSCAPE}, INT_MIN, INT_MAX, FLAGS, "passthrough" },

    { NULL }
};

static const AVClass vttranspose_class = {
    .class_name = "vttranspose",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad vttranspose_inputs[] = {
    {
        .name        = "default",
        .type        = AVMEDIA_TYPE_VIDEO,
        .filter_frame = vttranspose_filter_frame,
        .get_buffer.video = vttranspose_get_video_buffer,
    },
};

static const AVFilterPad vttranspose_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
};

const AVFilter ff_vf_transpose_videotoolbox = {
    .name      = "transpose_videotoolbox",
    .description = NULL_IF_CONFIG_SMALL("GPU-accelerated video rotation and transposition"),

    .init        = vttranspose_init,
    .uninit      = vttranspose_uninit,

    .priv_size = TRANSPOSE_VT_CTX_SIZE,
    .priv_class = &vttranspose_class,

    FILTER_INPUTS(vttranspose_inputs),
    FILTER_OUTPUTS(vttranspose_outputs),

    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_VIDEOTOOLBOX),

    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
