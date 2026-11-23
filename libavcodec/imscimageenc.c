/*
 * IMSC1 Image subtitle encoder
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
 * IMSC1 Image subtitle encoder: turns bitmap subtitles (PGS, DVD, DVB...)
 * into PNG images with their position, for the imsc_image muxer, which writes
 * them as a TTML document of the IMSC1 Image profile.
 *
 * Each packet holds one subtitle, all integers big-endian:
 *   u16 canvas width, u16 canvas height (0 if unknown)
 *   u16 number of images (0 clears the screen)
 *   for each image: u16 x, u16 y, u16 width, u16 height,
 *                   u32 PNG size, PNG data
 *
 * @see https://www.w3.org/TR/ttml-imsc1.0.1/#image-profile
 */

#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "avcodec.h"
#include "bytestream.h"
#include "codec_internal.h"

#define IMSC_IMAGE_HEADER_SIZE 6
#define IMSC_IMAGE_ENTRY_SIZE 12

typedef struct IMSCImageEncContext {
    const AVCodec *png;
    AVPacket *pkt;
} IMSCImageEncContext;

static av_cold int imsc_image_encode_init(AVCodecContext *avctx)
{
    IMSCImageEncContext *s = avctx->priv_data;

    s->png = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!s->png) {
        av_log(avctx, AV_LOG_ERROR, "The PNG encoder is not available.\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }
    s->pkt = av_packet_alloc();
    if (!s->pkt)
        return AVERROR(ENOMEM);
    return 0;
}

static av_cold int imsc_image_encode_close(AVCodecContext *avctx)
{
    IMSCImageEncContext *s = avctx->priv_data;

    av_packet_free(&s->pkt);
    return 0;
}

/* Whether any pixel of the rectangle is visible. */
static int rect_is_visible(const AVSubtitleRect *rect)
{
    const uint32_t *palette = (const uint32_t *)rect->data[1];
    uint8_t visible[256] = { 0 };
    int any = 0;

    for (int i = 0; i < FFMIN(rect->nb_colors, 256); i++)
        any |= visible[i] = !!(palette[i] >> 24);
    if (!any)
        return 0;

    for (int y = 0; y < rect->h; y++) {
        const uint8_t *line = rect->data[0] + y * rect->linesize[0];
        for (int x = 0; x < rect->w; x++)
            if (visible[line[x]])
                return 1;
    }
    return 0;
}

/* Encodes a palettized rectangle as PNG into s->pkt. */
static int encode_png(AVCodecContext *avctx, const AVSubtitleRect *rect)
{
    IMSCImageEncContext *s = avctx->priv_data;
    AVCodecContext *png_ctx;
    AVFrame *frame = NULL;
    int ret;

    png_ctx = avcodec_alloc_context3(s->png);
    if (!png_ctx)
        return AVERROR(ENOMEM);
    png_ctx->width     = rect->w;
    png_ctx->height    = rect->h;
    png_ctx->pix_fmt   = AV_PIX_FMT_PAL8;
    png_ctx->time_base = (AVRational){ 1, 1 };
    /* Subtitle bitmaps are small and few: favour the size of the images. */
    png_ctx->compression_level = 9;
    ret = avcodec_open2(png_ctx, s->png, NULL);
    if (ret < 0)
        goto end;

    frame = av_frame_alloc();
    if (!frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    frame->format = AV_PIX_FMT_PAL8;
    frame->width  = rect->w;
    frame->height = rect->h;
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
        goto end;
    av_image_copy_plane(frame->data[0], frame->linesize[0],
                        rect->data[0], rect->linesize[0], rect->w, rect->h);
    memset(frame->data[1], 0, AVPALETTE_SIZE);
    memcpy(frame->data[1], rect->data[1],
           FFMIN(rect->nb_colors, 256) * sizeof(uint32_t));

    ret = avcodec_send_frame(png_ctx, frame);
    if (ret < 0)
        goto end;
    av_packet_unref(s->pkt);
    ret = avcodec_receive_packet(png_ctx, s->pkt);

end:
    av_frame_free(&frame);
    avcodec_free_context(&png_ctx);
    return ret;
}

static int imsc_image_encode_frame(AVCodecContext *avctx, uint8_t *buf,
                                   int buf_size, const AVSubtitle *sub)
{
    IMSCImageEncContext *s = avctx->priv_data;
    PutByteContext pb;
    int nb_images = 0, ret;

    if (buf_size < IMSC_IMAGE_HEADER_SIZE)
        return AVERROR_BUFFER_TOO_SMALL;
    bytestream2_init_writer(&pb, buf, buf_size);
    bytestream2_put_be16(&pb, FFMIN(avctx->width,  UINT16_MAX));
    bytestream2_put_be16(&pb, FFMIN(avctx->height, UINT16_MAX));
    bytestream2_put_be16(&pb, 0); /* number of images, set below */

    for (unsigned i = 0; i < sub->num_rects; i++) {
        const AVSubtitleRect *rect = sub->rects[i];

        if (rect->type != SUBTITLE_BITMAP) {
            av_log(avctx, AV_LOG_ERROR,
                   "Only bitmap subtitles are supported, convert text "
                   "subtitles with the ttml encoder.\n");
            return AVERROR(EINVAL);
        }
        if (rect->w <= 0 || rect->h <= 0 || !rect->data[0] || !rect->data[1] ||
            rect->x < 0 || rect->y < 0 ||
            rect->x > UINT16_MAX || rect->y > UINT16_MAX ||
            rect->w > UINT16_MAX || rect->h > UINT16_MAX)
            continue;
        if (!rect_is_visible(rect))
            continue;

        ret = encode_png(avctx, rect);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Could not encode the image as PNG.\n");
            return ret;
        }
        if (bytestream2_get_bytes_left_p(&pb) <
            IMSC_IMAGE_ENTRY_SIZE + (int64_t)s->pkt->size) {
            av_log(avctx, AV_LOG_ERROR,
                   "The subtitle images are too large for the output buffer.\n");
            av_packet_unref(s->pkt);
            return AVERROR_BUFFER_TOO_SMALL;
        }
        bytestream2_put_be16(&pb, rect->x);
        bytestream2_put_be16(&pb, rect->y);
        bytestream2_put_be16(&pb, rect->w);
        bytestream2_put_be16(&pb, rect->h);
        bytestream2_put_be32(&pb, s->pkt->size);
        bytestream2_put_bufferu(&pb, s->pkt->data, s->pkt->size);
        av_packet_unref(s->pkt);
        nb_images++;
    }

    AV_WB16(buf + 4, nb_images);
    return bytestream2_tell_p(&pb);
}

const FFCodec ff_imsc_image_encoder = {
    .p.name         = "imsc_image",
    CODEC_LONG_NAME("IMSC1 Image subtitles (PNG)"),
    .p.type         = AVMEDIA_TYPE_SUBTITLE,
    .p.id           = AV_CODEC_ID_IMSC_IMAGE,
    .priv_data_size = sizeof(IMSCImageEncContext),
    .init           = imsc_image_encode_init,
    FF_CODEC_ENCODE_SUB_CB(imsc_image_encode_frame),
    .close          = imsc_image_encode_close,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
