/*
 * EMERGETECH VPE Video Decoder Common interface
 * EMERGETECH VPE Video Decoder Common interface
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

#include "internal.h"
#include "vpe_dec_common.h"

/**
 * Initialize hw frame and device
 *
 * The avctx->hw_frames_ctx is the reference to the AVHWFramesContext.
 * Create the hw_frame_ctx and initialize hw_frame_ctx.
 */
static int vpe_dec_init_hwctx(AVCodecContext *avctx)
{
    int ret = 0;
    AVHWFramesContext *hwframe_ctx;

    if (!avctx->hw_frames_ctx) {
        if (avctx->hw_device_ctx) {
            avctx->hw_frames_ctx = av_hwframe_ctx_alloc(avctx->hw_device_ctx);
            if (!avctx->hw_frames_ctx) {
                av_log(avctx, AV_LOG_ERROR, "av_hwframe_ctx_alloc failed\n");
                ret = AVERROR(ENOMEM);
                goto error;
            }
        } else {
            av_log(avctx, AV_LOG_ERROR, "No hw frame/device available\n");
            ret = AVERROR(EINVAL);
            goto error;
        }
    }

    hwframe_ctx = (AVHWFramesContext *)avctx->hw_frames_ctx->data;
    hwframe_ctx->format = AV_PIX_FMT_VPE;
    if (avctx->bits_per_raw_sample == 10) {
        hwframe_ctx->sw_format = AV_PIX_FMT_P010LE;
    } else {
        if (avctx->pix_fmt == AV_PIX_FMT_YUV420P10LE) {
            hwframe_ctx->sw_format = AV_PIX_FMT_P010LE;
        } else {
            hwframe_ctx->sw_format = AV_PIX_FMT_NV12;
        }
    }
    hwframe_ctx->width  = avctx->width;
    hwframe_ctx->height = avctx->height;
    if ((ret = av_hwframe_ctx_init(avctx->hw_frames_ctx)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "av_hwframe_ctx_init failed\n");
        return ret;
    }
    return 0;

error:
    av_log(avctx, AV_LOG_ERROR, "vpe_dec_init_hwctx failed\n");
    return ret;
}

/**
 * Notify the external decoder to release frame buffer
 */
static void vpe_decode_picture_consume(VpeDecCtx *dec_ctx, VpiFrame *vpi_frame)
{
    VpiCtrlCmdParam cmd_param;

    // make decoder release DPB
    cmd_param.cmd  = VPI_CMD_DEC_PIC_CONSUME;
    cmd_param.data = (void *)vpi_frame;
    dec_ctx->vpi->control(dec_ctx->ctx, (void *)&cmd_param, NULL);
}

int ff_vpe_decode_init(AVCodecContext *avctx, VpiPlugin type)
{
    AVHWFramesContext *hwframe_ctx;
    AVVpeFramesContext *vpeframe_ctx;
    AVHWDeviceContext *hwdevice_ctx;
    AVVpeDeviceContext *vpedev_ctx;
    VpeDecCtx *dec_ctx = (VpeDecCtx *)avctx->priv_data;
    VpiCtrlCmdParam cmd_param;
    int ret;

    // Init vpe hwcontext
    ret = vpe_dec_init_hwctx(avctx);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "vpe init hwctx failure\n");
        return ret;
    }

    hwframe_ctx  = (AVHWFramesContext *)avctx->hw_frames_ctx->data;
    vpeframe_ctx = (AVVpeFramesContext *)hwframe_ctx->hwctx;
    hwdevice_ctx = (AVHWDeviceContext *)avctx->hw_device_ctx->data;
    vpedev_ctx   = (AVVpeDeviceContext *)hwdevice_ctx->hwctx;

    // Create the VPE context
    ret = vpi_create(&dec_ctx->ctx, &dec_ctx->vpi, vpedev_ctx->device, type);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "vpi create failure error=%s(%d)\n",
               vpi_error_str(ret), ret);
        return AVERROR_EXTERNAL;
    }

    // get the decoder init option struct
    cmd_param.cmd = VPI_CMD_DEC_INIT_OPTION;
    ret = dec_ctx->vpi->control(dec_ctx->ctx, (void *)&cmd_param,
                                (void *)&dec_ctx->dec_setting);
    if (ret)
        return AVERROR(ENOMEM);

    dec_ctx->avctx                   = avctx;
    dec_ctx->dec_setting->pp_setting = dec_ctx->pp_setting;
    dec_ctx->dec_setting->transcode  = dec_ctx->transcode;
    dec_ctx->dec_setting->frame      = vpeframe_ctx->frame;
    dec_ctx->dec_setting->src_width  = avctx->width;
    dec_ctx->dec_setting->src_height = avctx->height;
    dec_ctx->dec_setting->frmrate_n  = avctx->framerate.num;
    dec_ctx->dec_setting->frmrate_d  = avctx->framerate.den;
    avctx->pix_fmt                   = AV_PIX_FMT_VPE;

    // Initialize the VPE decoder
    ret = dec_ctx->vpi->init(dec_ctx->ctx, dec_ctx->dec_setting);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "vpi decode init failure, error=%s(%d)\n",
               vpi_error_str(ret), ret);
        return AVERROR_EXTERNAL;
    }

    // get the packet buffer struct info from external decoder
    // to store the avpkt buf info
    cmd_param.cmd = VPI_CMD_DEC_GET_STRM_BUF_PKT;
    ret = dec_ctx->vpi->control(dec_ctx->ctx, (void *)&cmd_param,
                                (void *)&dec_ctx->buffered_pkt);
    if (ret)
        return AVERROR(ENOMEM);

    return 0;
}

/**
 * clear the unsed frames
 */
static void vpe_clear_unused_frames(VpeDecCtx *dec_ctx)
{
    VpeDecFrame *cur_frame = dec_ctx->frame_list;

    while (cur_frame) {
        if (cur_frame->used
            && !cur_frame->vpi_frame->locked) {
            vpe_decode_picture_consume(dec_ctx, cur_frame->vpi_frame);
            cur_frame->used = 0;
        }
        cur_frame = cur_frame->next;
    }
}

/**
 * alloc frame from hwcontext for external codec
 */
static int vpe_alloc_frame(AVCodecContext *avctx, VpeDecFrame *dec_frame)
{
    int ret;

    ret = ff_get_buffer(avctx, dec_frame->av_frame, AV_GET_BUFFER_FLAG_REF);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "return ret %d\n", ret);
        return ret;
    }

    dec_frame->vpi_frame = (VpiFrame*)dec_frame->av_frame->data[0];
    dec_frame->used      = 1;

    return 0;
}

/**
 * get frame from the linked list
 */
static int vpe_get_frame(AVCodecContext *avctx, VpiFrame **vpi_frame)
{
    VpeDecCtx *dec_ctx = (VpeDecCtx *)avctx->priv_data;
    VpeDecFrame *dec_frame, **last;
    int ret;

    vpe_clear_unused_frames(dec_ctx);

    dec_frame = dec_ctx->frame_list;
    last      = &dec_ctx->frame_list;
    while (dec_frame) {
        if (!dec_frame->used) {
            ret = vpe_alloc_frame(avctx, dec_frame);
            if (ret < 0)
                return ret;
            *vpi_frame = dec_frame->vpi_frame;
            return 0;
        }

        last      = &dec_frame->next;
        dec_frame = dec_frame->next;
    }

    dec_frame = av_mallocz(sizeof(*dec_frame));
    if (!dec_frame)
        return AVERROR(ENOMEM);
    dec_frame->av_frame = av_frame_alloc();
    if (!dec_frame->av_frame) {
        av_freep(&dec_frame);
        return AVERROR(ENOMEM);
    }
    *last = dec_frame;

    ret = vpe_alloc_frame(avctx, dec_frame);
    if (ret < 0)
        return ret;

    *vpi_frame = dec_frame->vpi_frame;

    return 0;
}

/**
 * find frame from the linked list
 */
static VpeDecFrame *vpe_find_frame(VpeDecCtx *dec_ctx, VpiFrame *vpi_frame)
{
    VpeDecFrame *cur_frame = dec_ctx->frame_list;

    while (cur_frame) {
        if (vpi_frame == cur_frame->vpi_frame)
            return cur_frame;
        cur_frame = cur_frame->next;
    }
    return NULL;
}

/**
 * Output the frame raw data
 */
static int vpe_output_frame(AVCodecContext *avctx, VpiFrame *vpi_frame,
                            AVFrame *out_frame)
{
    VpeDecCtx *dec_ctx     = (VpeDecCtx *)avctx->priv_data;
    VpeDecFrame *dec_frame = NULL;
    AVFrame *src_frame     = NULL;

    dec_frame = vpe_find_frame(dec_ctx, vpi_frame);
    if (dec_frame == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Can't find matched frame from pool\n");
        return AVERROR_BUG;
    }
    src_frame = dec_frame->av_frame;
    av_frame_move_ref(out_frame, src_frame);

    out_frame->linesize[0]           = vpi_frame->linesize[0];
    out_frame->linesize[1]           = vpi_frame->linesize[1];
    out_frame->linesize[2]           = vpi_frame->linesize[2];
    out_frame->key_frame             = vpi_frame->key_frame;
    out_frame->pts                   = vpi_frame->pts;
    out_frame->pkt_dts               = vpi_frame->pkt_dts;
    out_frame->best_effort_timestamp = out_frame->pts;

    return 0;
}

/**
 * receive VpiFrame from external decoder
 */
static int vpe_dec_receive(AVCodecContext *avctx, AVFrame *frame)
{
    VpeDecCtx *dec_ctx  = (VpeDecCtx *)avctx->priv_data;
    VpiFrame *vpi_frame = NULL;
    int ret;

    vpe_clear_unused_frames(dec_ctx);
    ret = dec_ctx->vpi->decode_get_frame(dec_ctx->ctx, (void *)&vpi_frame);
    if (ret == 1) {
        // get one output frame
        if (0 == vpe_output_frame(avctx, vpi_frame, frame)) {
            return 0;
        } else {
            return AVERROR_EXTERNAL;
        }
    } else if (ret == 2) {
        return AVERROR_EOF;
    } else {
        return AVERROR(EAGAIN);
    }
}

/**
 * release avpkt buf which is released by external decoder
 */
static int vpe_release_stream_mem(VpeDecCtx *dec_ctx)
{
    VpiCtrlCmdParam cmd_param;
    AVBufferRef *ref = NULL;
    VpeDecPacket *vpe_packet;
    int ret, i;

    cmd_param.cmd = VPI_CMD_DEC_GET_USED_STRM_MEM;
     /* get the used packet buffer info from external decoder */
    ret = dec_ctx->vpi->control(dec_ctx->ctx, (void *)&cmd_param, (void *)&ref);
    if (ret)
        return AVERROR_EXTERNAL;
    if (ref) {
        for (i = 0; i < MAX_WAIT_DEPTH; i++) {
            if (dec_ctx->packet_buf_wait_list[i].state == 1) {
                vpe_packet = &dec_ctx->packet_buf_wait_list[i];
                if (vpe_packet->buf_ref == ref)
                    break;
            }
        }
        if (i == MAX_WAIT_DEPTH) {
            av_log(dec_ctx, AV_LOG_ERROR,
                   "buf_ref %p not matched\n", ref);
            return AVERROR(EINVAL);
        }

        vpe_packet->state = 0;
        /* unref the input avpkt buffer */
        av_buffer_unref(&ref);
        vpe_packet->buf_ref = NULL;
    }

    return 0;
}

int ff_vpe_decode_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    VpeDecCtx *dec_ctx     = (VpeDecCtx *)avctx->priv_data;
    AVPacket avpkt         = { 0 };
    AVBufferRef *ref       = NULL;
    VpiFrame *in_vpi_frame = NULL;
    VpiCtrlCmdParam cmd_param;
    VpeDecPacket *vpe_packet;
    int strm_buf_count, frame_buf_req;
    int ret, i;

    ret = vpe_release_stream_mem(dec_ctx);
    if (ret < 0) {
        return ret;
    }
    /* poll for new frame */
    ret = vpe_dec_receive(avctx, frame);
    if (ret != AVERROR(EAGAIN)) {
        return ret;
    }

    /* feed decoder */
    while (1) {
        cmd_param.cmd  = VPI_CMD_DEC_STRM_BUF_COUNT;
        cmd_param.data = NULL;
        ret = dec_ctx->vpi->control(dec_ctx->ctx,
                        (void*)&cmd_param, (void *)&strm_buf_count);
        if (ret)
            return AVERROR_EXTERNAL;
        if (strm_buf_count == -1) {
            /* no space, block for an output frame to get space */
            ret = vpe_dec_receive(avctx, frame);
            if (ret != AVERROR(EAGAIN)) {
                return ret;
            } else {
                continue;
            }
        }

        ret = vpe_release_stream_mem(dec_ctx);
        if (ret < 0) {
            return ret;
        }

        /* fetch new packet or eof */
        ret = ff_decode_get_packet(avctx, &avpkt);
        if (ret == 0) {
            dec_ctx->buffered_pkt->data     = avpkt.data;
            dec_ctx->buffered_pkt->size     = avpkt.size;
            dec_ctx->buffered_pkt->pts      = avpkt.pts;
            dec_ctx->buffered_pkt->pkt_dts  = avpkt.dts;
            dec_ctx->buffered_pkt->duration = avpkt.duration;
            ref                             = av_buffer_ref(avpkt.buf);
            if (!ref) {
                av_packet_unref(&avpkt);
                return AVERROR(ENOMEM);
            }
            /* ref the avpkt buffer
               feed the packet buffer related info to external decoder */
            dec_ctx->buffered_pkt->opaque  = (void *)ref;
            av_packet_unref(&avpkt);

            for (i = 0; i < MAX_WAIT_DEPTH; i++) {
                if (dec_ctx->packet_buf_wait_list[i].state == 0) {
                    vpe_packet = &dec_ctx->packet_buf_wait_list[i];
                    break;
                }
            }
            if (i == MAX_WAIT_DEPTH) {
                return AVERROR_BUFFER_TOO_SMALL;
            }
            vpe_packet->state   = 1;
            vpe_packet->buf_ref = ref;

            cmd_param.cmd  = VPI_CMD_DEC_GET_FRAME_BUFFER_REQUEST;
            cmd_param.data = NULL;
            ret = dec_ctx->vpi->control(dec_ctx->ctx,
                            (void*)&cmd_param, (void *)&frame_buf_req);
            if (ret)
                return AVERROR_EXTERNAL;
            if (frame_buf_req) {
                /* get frame buffer from pool */
                ret = vpe_get_frame(avctx, &in_vpi_frame);
                if (ret < 0) {
                    return ret;
                }

                cmd_param.cmd  = VPI_CMD_DEC_SET_FRAME_BUFFER;
                cmd_param.data = (void *)in_vpi_frame;
                ret = dec_ctx->vpi->control(dec_ctx->ctx,
                                            (void *)&cmd_param, NULL);
                if (ret)
                    return AVERROR_EXTERNAL;
            }
        } else if (ret == AVERROR_EOF) {
            ret = dec_ctx->vpi->decode_put_packet(dec_ctx->ctx,
                (void *)dec_ctx->buffered_pkt);
            if (ret < 0) {
                return AVERROR_EXTERNAL;
            } else {
                return AVERROR(EAGAIN);
            }
        } else if (ret == AVERROR(EAGAIN)) {
            return vpe_dec_receive(avctx, frame);
        } else if (ret < 0) {
            return ret;
        }

        /* try to flush packet data */
        if (dec_ctx->buffered_pkt->size > 0) {
            ret = dec_ctx->vpi->decode_put_packet(dec_ctx->ctx,
                    (void *)dec_ctx->buffered_pkt);
            if (ret > 0) {
                dec_ctx->buffered_pkt->size -= ret;
                if (dec_ctx->buffered_pkt->size != 0) {
                    return AVERROR_EXTERNAL;
                }
                return vpe_dec_receive(avctx, frame);
            } else {
                return AVERROR_EXTERNAL;
            }
        }
    }

    return AVERROR(EAGAIN);
}

static av_cold void vpe_dec_consume_flush(VpeDecCtx *dec_ctx)
{
    VpeDecPacket *vpe_packet = NULL;
    int i;

    for (i = 0; i < MAX_WAIT_DEPTH; i++) {
        if (dec_ctx->packet_buf_wait_list[i].state == 1) {
            vpe_packet = &dec_ctx->packet_buf_wait_list[i];
            if (vpe_packet->buf_ref) {
                av_buffer_unref(&vpe_packet->buf_ref);
            }
            vpe_packet->state = 0;
        }
    }
}

av_cold int ff_vpe_decode_close(AVCodecContext *avctx)
{
    AVHWDeviceContext *hwdevice_ctx;
    AVVpeDeviceContext *vpedev_ctx;
    VpeDecCtx *dec_ctx = avctx->priv_data;
    VpeDecFrame *cur_frame;
    VpiCtrlCmdParam cmd_param;
    int ret;

    hwdevice_ctx = (AVHWDeviceContext *)avctx->hw_device_ctx->data;
    vpedev_ctx   = (AVVpeDeviceContext *)hwdevice_ctx->hwctx;

    if (dec_ctx->buffered_pkt) {
        free(dec_ctx->buffered_pkt);
    }
    cmd_param.cmd  = VPI_CMD_DEC_CLEAR_FRAME_BUFFER;
    ret = dec_ctx->vpi->control(dec_ctx->ctx,
                                (void *)&cmd_param, NULL);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "decoder clear frame buffer failure\n");
        return AVERROR_EXTERNAL;
    }
    vpe_clear_unused_frames(dec_ctx);
    if (dec_ctx->ctx == NULL)
        return 0;
    dec_ctx->vpi->close(dec_ctx->ctx);
    vpe_dec_consume_flush(dec_ctx);

    cur_frame = dec_ctx->frame_list;
    while (cur_frame) {
        dec_ctx->frame_list = cur_frame->next;
        av_frame_free(&cur_frame->av_frame);
        av_freep(&cur_frame);
        cur_frame = dec_ctx->frame_list;
    }
    if (dec_ctx->dec_setting)
        free(dec_ctx->dec_setting);
    if (vpi_destroy(dec_ctx->ctx, vpedev_ctx->device)) {
        av_log(avctx, AV_LOG_ERROR, "decoder vpi_destroy failure\n");
        return AVERROR_EXTERNAL;
    }

    return 0;
}

#define OFFSET(x) offsetof(VpeDecCtx, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
const AVOption vpe_decode_options[] = {
    { "low_res",
      "set output number and at most four output downscale configuration",
      OFFSET(pp_setting),
      AV_OPT_TYPE_STRING,
      { .str = NULL },
      0,
      0,
      VD },
    { "transcode",
      "enable/disable transcoding",
      OFFSET(transcode),
      AV_OPT_TYPE_BOOL,
      { .i64 = 0 },
      0,
      1,
      VD },
    { NULL },
};
