/*
 * EMERGETECH VPE Hwdownload Filter
 * EMERGETECH VPE Hwdownload Filter
 * Copyright (C) 2020 EMERGETECH Company Ltd.
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


#include "libavutil/buffer.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_vpe.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct HWDownloadVpeContext {
    const AVClass *class;

    AVBufferRef       *hwframes_ref;
    AVHWFramesContext *hwframes;
    AVVpeFramesContext *hwframes_vpe;

    VpiCtx hwdownload_ctx;
    VpiApi *hwdownload_vpi;
} HWDownloadVpeContext;

static av_cold int hwdownload_vpe_init(AVFilterContext *avctx)
{
    HWDownloadVpeContext *ctx = avctx->priv;

    return 0;
}

static int hwdownload_vpe_query_formats(AVFilterContext *avctx)
{
    AVFilterFormats *fmts;
    int err;

    if ((err = ff_formats_pixdesc_filter(&fmts, AV_PIX_FMT_FLAG_HWACCEL, 0)) ||
        (err = ff_formats_ref(fmts, &avctx->inputs[0]->outcfg.formats))      ||
        (err = ff_formats_pixdesc_filter(&fmts, 0, AV_PIX_FMT_FLAG_HWACCEL)) ||
        (err = ff_formats_ref(fmts, &avctx->outputs[0]->incfg.formats)))
        return err;

    return 0;
}

static int hwdownload_vpe_config_input(AVFilterLink *inlink)
{
    AVFilterContext *avctx = inlink->dst;
    HWDownloadVpeContext *ctx = avctx->priv;

    av_buffer_unref(&ctx->hwframes_ref);

    if (!inlink->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "The input must have a hardware frame "
               "reference.\n");
        return AVERROR(EINVAL);
    }

    ctx->hwframes_ref = av_buffer_ref(inlink->hw_frames_ctx);
    if (!ctx->hwframes_ref)
        return AVERROR(ENOMEM);

    ctx->hwframes = (AVHWFramesContext*)ctx->hwframes_ref->data;
    ctx->hwframes_vpe = ctx->hwframes->hwctx;

    return 0;
}

static int hwdownload_vpe_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink *inlink   = avctx->inputs[0];
    HWDownloadVpeContext *ctx = avctx->priv;
    enum AVPixelFormat *formats;
    int err, i, found;

    if (!ctx->hwframes_ref)
        return AVERROR(EINVAL);

    err = av_hwframe_transfer_get_formats(ctx->hwframes_ref,
                                          AV_HWFRAME_TRANSFER_DIRECTION_FROM,
                                          &formats, 0);
    if (err < 0)
        return err;

    found = 0;
    for (i = 0; formats[i] != AV_PIX_FMT_NONE; i++) {
        if (formats[i] == outlink->format) {
            found = 1;
            break;
        }
    }
    av_freep(&formats);

    if (!found) {
        av_log(ctx, AV_LOG_ERROR, "Invalid output format %s for hwframe "
               "download.\n", av_get_pix_fmt_name(outlink->format));
        return AVERROR(EINVAL);
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;

    return 0;
}

static void free_frame_buffer(HWDownloadVpeContext *ctx, void *data)
{
    VpiCtrlCmdParam cmd_param;

    cmd_param.cmd  = VPI_CMD_HWDW_FREE_BUF;
    cmd_param.data = data;
    ctx->hwdownload_vpi->control(ctx->hwdownload_ctx, (void *)&cmd_param, NULL);
}

static int hwdownload_vpe_filter_frame(AVFilterLink *link, AVFrame *input)
{
    AVFilterContext *avctx = link->dst;
    AVFilterLink  *outlink = avctx->outputs[0];
    HWDownloadVpeContext *ctx = avctx->priv;
    AVFrame *output = NULL;
    VpiFrame *in_frame, out_frame;
    int err;

    if (!ctx->hwframes_ref || !input->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Input frames must have hardware context.\n");
        err = AVERROR(EINVAL);
        goto fail;
    }
    if ((void*)ctx->hwframes != input->hw_frames_ctx->data) {
        av_log(ctx, AV_LOG_ERROR, "Input frame is not the in the configured "
               "hwframe context.\n");
        err = AVERROR(EINVAL);
        goto fail;
    }

    output = av_frame_alloc();
    if (!output) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    output->data[0] = av_malloc(ctx->hwframes_vpe->pic_info_size);
    if (!output->data[0]) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    memset(output->data[0], 0, ctx->hwframes_vpe->pic_info_size);
    output->buf[0] = av_buffer_create(output->data[0], ctx->hwframes_vpe->pic_info_size,
                                      NULL, NULL, AV_BUFFER_FLAG_READONLY);
    if (!output->buf[0]) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = av_hwframe_transfer_data(output, input, 0);
    if (err < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to download frame: %d.\n", err);
        goto fail;
    }

    av_free(output->data[0]);

    if (!ctx->hwdownload_ctx) {
        ctx->hwdownload_ctx = ctx->hwframes_vpe->hwdownload_ctx;
    }
    if (!ctx->hwdownload_vpi) {
        ctx->hwdownload_vpi = ctx->hwframes_vpe->hwdownload_vpi;
    }
    in_frame = (VpiFrame *)input->data[0];
    out_frame.linesize[0] = 0;
    out_frame.linesize[1] = 0;

    ctx->hwdownload_vpi->process(ctx->hwdownload_ctx,
                                      in_frame, &out_frame);
    output->data[0] = out_frame.data[0];
    output->data[1] = out_frame.data[1];
    output->linesize[1] = out_frame.linesize[1];
    output->linesize[0] = out_frame.linesize[0];
    if (out_frame.raw_format == VPI_FMT_NV12) {
        output->format = AV_PIX_FMT_NV12;
    } else {
        output->format = AV_PIX_FMT_P010LE;
    }

    output->buf[0] = av_buffer_create(output->data[0], out_frame.src_width,
                                      free_frame_buffer, ctx, AV_BUFFER_FLAG_READONLY);
    output->buf[1] = av_buffer_create(output->data[1], out_frame.src_height,
                                      free_frame_buffer, ctx, AV_BUFFER_FLAG_READONLY);
    output->width  = outlink->w;
    output->height = outlink->h;

    err = av_frame_copy_props(output, input);
    if (err < 0)
        goto fail;

    av_frame_free(&input);

    return ff_filter_frame(avctx->outputs[0], output);

fail:
    av_frame_free(&input);
    av_frame_free(&output);
    return err;
}

static av_cold void hwdownload_vpe_uninit(AVFilterContext *avctx)
{
    HWDownloadVpeContext *ctx = avctx->priv;

    av_buffer_unref(&ctx->hwframes_ref);
}

static const AVClass hwdownload_vpe_class = {
    .class_name = "hwdownload_vpe",
    .item_name  = av_default_item_name,
    .option     = NULL,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad hwdownload_vpe_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = hwdownload_vpe_config_input,
        .filter_frame = hwdownload_vpe_filter_frame,
    },
    { NULL }
};

static const AVFilterPad hwdownload_vpe_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = hwdownload_vpe_config_output,
    },
    { NULL }
};

AVFilter ff_vf_hwdownload_vpe = {
    .name          = "hwdownload_vpe",
    .description   = NULL_IF_CONFIG_SMALL("Download a vpe hardware framehwdownlaod_vpe_init to a normal frame"),
    .init          = hwdownload_vpe_init,
    .uninit        = hwdownload_vpe_uninit,
    .query_formats = hwdownload_vpe_query_formats,
    .priv_size     = sizeof(HWDownloadVpeContext),
    .priv_class    = &hwdownload_vpe_class,
    .inputs        = hwdownload_vpe_inputs,
    .outputs       = hwdownload_vpe_outputs,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
