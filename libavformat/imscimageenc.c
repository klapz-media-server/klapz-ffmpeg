/*
 * IMSC1 Image subtitle muxer
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
 * IMSC1 Image subtitle muxer: writes the images of the imsc_image encoder as
 * a TTML document of the IMSC1 Image profile, with one PNG file per image
 * next to the document, or as SMPTE-TT with the images embedded in Base64.
 *
 * Bitmap subtitles such as PGS have no end time: a subtitle is shown until
 * the next one, which may be empty to clear the screen.  So each image ends
 * at the next subtitle, or at the end of its duration if it has one, and the
 * document is written at the end, once all the regions are known.
 *
 * @see https://www.w3.org/TR/ttml-imsc1.0.1/#image-profile
 * @see SMPTE ST 2052-1
 */

#include "libavutil/avstring.h"
#include "libavutil/base64.h"
#include "libavutil/bprint.h"
#include "libavutil/dict.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavcodec/bytestream.h"
#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "mux.h"

#define IMSC_IMAGE_HEADER_SIZE 6
#define IMSC_IMAGE_ENTRY_SIZE 12

/* Beyond this, a duration means "until the next subtitle", like the
 * UINT32_MAX ms of the PGS decoder. */
#define IMSC_IMAGE_MAX_DURATION_MS INT32_MAX

typedef struct IMSCImageRegion {
    int x, y, w, h;
} IMSCImageRegion;

typedef struct IMSCImageEvent {
    int64_t start;
    int64_t end;            ///< INT64_MAX until the next subtitle
    int region;
    uint8_t *png;
    int png_size;
} IMSCImageEvent;

typedef struct IMSCImageMuxContext {
    const AVClass *class;
    int embed_images;
    char *image_prefix;
    int64_t last_duration;

    int canvas_w, canvas_h;
    int max_x, max_y;

    IMSCImageRegion *regions;
    int nb_regions;
    unsigned regions_size;

    IMSCImageEvent *pending;
    int nb_pending;
    unsigned pending_size;

    int nb_images;
    char *image_dir;        ///< directory of the document, with its separator
    AVBPrint body;
    AVBPrint metadata;
} IMSCImageMuxContext;

static int imsc_image_init(AVFormatContext *s)
{
    IMSCImageMuxContext *imsc = s->priv_data;
    AVStream *st = s->streams[0];

    av_bprint_init(&imsc->body, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprint_init(&imsc->metadata, 0, AV_BPRINT_SIZE_UNLIMITED);

    if (st->codecpar->codec_id != AV_CODEC_ID_IMSC_IMAGE) {
        av_log(s, AV_LOG_ERROR, "Only the imsc_image codec is supported.\n");
        return AVERROR(EINVAL);
    }
    avpriv_set_pts_info(st, 64, 1, 1000);

    if (!imsc->embed_images) {
        const char *url = s->url;
        const char *base = av_basename(url);
        const char *ext;

        if (!*url || !strcmp(url, "-") || av_strstart(url, "pipe:", NULL)) {
            av_log(s, AV_LOG_ERROR, "The images can only be written next to "
                   "a file: use -embed_images 1 to embed them in the document.\n");
            return AVERROR(EINVAL);
        }
        imsc->image_dir = av_strndup(url, base - url);
        if (!imsc->image_dir)
            return AVERROR(ENOMEM);
        if (!imsc->image_prefix) {
            ext = strrchr(base, '.');
            imsc->image_prefix = av_asprintf("%.*s_",
                                             ext && ext != base ? (int)(ext - base)
                                                                : (int)strlen(base),
                                             base);
            if (!imsc->image_prefix)
                return AVERROR(ENOMEM);
        }
    }
    return 0;
}

static int find_region(IMSCImageMuxContext *imsc, int x, int y, int w, int h)
{
    IMSCImageRegion *region;

    for (int i = 0; i < imsc->nb_regions; i++) {
        region = &imsc->regions[i];
        if (region->x == x && region->y == y && region->w == w && region->h == h)
            return i;
    }
    region = av_fast_realloc(imsc->regions, &imsc->regions_size,
                             (imsc->nb_regions + 1) * sizeof(*imsc->regions));
    if (!region)
        return AVERROR(ENOMEM);
    imsc->regions = region;
    imsc->regions[imsc->nb_regions] = (IMSCImageRegion){ x, y, w, h };
    imsc->max_x = FFMAX(imsc->max_x, x + w);
    imsc->max_y = FFMAX(imsc->max_y, y + h);
    return imsc->nb_regions++;
}

static void write_time(AVBPrint *bp, const char *name, int64_t ms)
{
    av_bprintf(bp, " %s=\"%02"PRId64":%02"PRId64":%02"PRId64".%03"PRId64"\"",
               name, ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}

/* Writes the image of an event whose end is known, and its div. */
static int write_event(AVFormatContext *s, IMSCImageEvent *ev)
{
    IMSCImageMuxContext *imsc = s->priv_data;
    int64_t start = FFMAX(ev->start, 0);
    int index, ret = 0;

    if (ev->end <= start)
        goto end;
    index = ++imsc->nb_images;

    av_bprintf(&imsc->body, "      <div region=\"r%d\"", ev->region + 1);
    write_time(&imsc->body, "begin", start);
    write_time(&imsc->body, "end", ev->end);

    if (imsc->embed_images) {
        int b64_size = AV_BASE64_SIZE(ev->png_size);
        char *b64 = av_malloc(b64_size);

        if (!b64) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        av_base64_encode(b64, b64_size, ev->png, ev->png_size);
        av_bprintf(&imsc->metadata,
                   "      <smpte:image xml:id=\"i%d\" imageType=\"PNG\" "
                   "encoding=\"Base64\">%s</smpte:image>\n", index, b64);
        av_free(b64);
        av_bprintf(&imsc->body, " smpte:backgroundImage=\"#i%d\"/>\n", index);
    } else {
        AVIOContext *pb = NULL;
        char *name = av_asprintf("%s%05d.png", imsc->image_prefix, index);
        char *path = name ? av_asprintf("%s%s", imsc->image_dir, name) : NULL;

        if (!path) {
            av_free(name);
            ret = AVERROR(ENOMEM);
            goto end;
        }
        ret = s->io_open(s, &pb, path, AVIO_FLAG_WRITE, NULL);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Could not open the image '%s'.\n", path);
        } else {
            avio_write(pb, ev->png, ev->png_size);
            ret = ff_format_io_close(s, &pb);
        }
        av_free(path);
        if (ret >= 0) {
            av_bprintf(&imsc->body, " smpte:backgroundImage=\"");
            av_bprint_escape(&imsc->body, name, NULL, AV_ESCAPE_MODE_XML,
                             AV_ESCAPE_FLAG_XML_DOUBLE_QUOTES);
            av_bprintf(&imsc->body, "\"/>\n");
        }
        av_free(name);
    }

end:
    av_freep(&ev->png);
    return ret;
}

/* Closes the pending events that end by time, or all of them if time is
 * INT64_MAX, except those which are still shown, identical, by the next
 * subtitle (keep[i] set). */
static int close_pending(AVFormatContext *s, int64_t time, const uint8_t *keep)
{
    IMSCImageMuxContext *imsc = s->priv_data;
    int nb_kept = 0, ret = 0;

    for (int i = 0; i < imsc->nb_pending; i++) {
        IMSCImageEvent *ev = &imsc->pending[i];

        if (keep && keep[i]) {
            imsc->pending[nb_kept++] = *ev;
            continue;
        }
        if (time == INT64_MAX && ev->end == INT64_MAX) {
            ev->end = ev->start + imsc->last_duration / 1000;
            av_log(s, AV_LOG_WARNING, "The last subtitle has no end, it is "
                   "shown for %"PRId64" ms.\n", imsc->last_duration / 1000);
        } else {
            ev->end = FFMIN(ev->end, time);
        }
        if (ret >= 0)
            ret = write_event(s, ev);
        else
            av_freep(&ev->png);
    }
    imsc->nb_pending = nb_kept;
    return ret;
}

static int imsc_image_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    IMSCImageMuxContext *imsc = s->priv_data;
    GetByteContext gb;
    uint8_t *keep = NULL;
    int64_t end = INT64_MAX;
    int canvas_w, canvas_h, nb_images, nb_old, ret = 0;

    if (pkt->pts == AV_NOPTS_VALUE) {
        av_log(s, AV_LOG_ERROR, "Subtitle packets must have a pts.\n");
        return AVERROR(EINVAL);
    }
    if (pkt->duration > 0 && pkt->duration < IMSC_IMAGE_MAX_DURATION_MS)
        end = pkt->pts + pkt->duration;

    bytestream2_init(&gb, pkt->data, pkt->size);
    if (bytestream2_get_bytes_left(&gb) < IMSC_IMAGE_HEADER_SIZE)
        return AVERROR_INVALIDDATA;
    canvas_w  = bytestream2_get_be16u(&gb);
    canvas_h  = bytestream2_get_be16u(&gb);
    nb_images = bytestream2_get_be16u(&gb);
    if (canvas_w && canvas_h) {
        imsc->canvas_w = FFMAX(imsc->canvas_w, canvas_w);
        imsc->canvas_h = FFMAX(imsc->canvas_h, canvas_h);
    }

    /* Whether each event, pending or new, is still shown after this
     * subtitle.  The new subtitle replaces the previous ones on the screen. */
    nb_old = imsc->nb_pending;
    keep = av_calloc(nb_old + nb_images + 1, 1);
    if (!keep)
        return AVERROR(ENOMEM);

    for (int i = 0; i < nb_images; i++) {
        IMSCImageEvent ev = { .start = pkt->pts, .end = end };
        unsigned size;
        int x, y, w, h, merged = 0;

        if (bytestream2_get_bytes_left(&gb) < IMSC_IMAGE_ENTRY_SIZE) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
        x = bytestream2_get_be16u(&gb);
        y = bytestream2_get_be16u(&gb);
        w = bytestream2_get_be16u(&gb);
        h = bytestream2_get_be16u(&gb);
        size = bytestream2_get_be32u(&gb);
        if (!w || !h || size > bytestream2_get_bytes_left(&gb)) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
        ev.png_size = size;
        ret = ev.region = find_region(imsc, x, y, w, h);
        if (ret < 0)
            goto fail;

        /* PGS sends the subtitles again at random access points: an image
         * still shown, unchanged, continues instead of starting again. */
        for (int j = 0; j < nb_old; j++) {
            IMSCImageEvent *prev = &imsc->pending[j];
            if (!keep[j] && prev->region == ev.region &&
                prev->end >= pkt->pts && prev->png_size == ev.png_size &&
                !memcmp(prev->png, gb.buffer, ev.png_size)) {
                prev->end = end;
                keep[j] = merged = 1;
                break;
            }
        }
        if (merged) {
            bytestream2_skipu(&gb, ev.png_size);
            continue;
        }

        ev.png = av_memdup(gb.buffer, ev.png_size);
        if (!ev.png) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        bytestream2_skipu(&gb, ev.png_size);
        {
            IMSCImageEvent *pending = av_fast_realloc(imsc->pending, &imsc->pending_size,
                                                      (imsc->nb_pending + 1) * sizeof(*pending));
            if (!pending) {
                av_free(ev.png);
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            imsc->pending = pending;
        }
        keep[imsc->nb_pending] = 1;
        imsc->pending[imsc->nb_pending++] = ev;
    }

    ret = close_pending(s, pkt->pts, keep);

fail:
    av_free(keep);
    return ret;
}

static int imsc_image_write_trailer(AVFormatContext *s)
{
    IMSCImageMuxContext *imsc = s->priv_data;
    const AVDictionaryEntry *lang = av_dict_get(s->streams[0]->metadata,
                                                "language", NULL, 0);
    AVCodecParameters *par = s->streams[0]->codecpar;
    AVIOContext *pb = s->pb;
    int canvas_w = imsc->canvas_w, canvas_h = imsc->canvas_h;
    int ret;

    ret = close_pending(s, INT64_MAX, NULL);
    if (ret < 0)
        return ret;

    if (!canvas_w || !canvas_h) {
        canvas_w = par->width;
        canvas_h = par->height;
    }
    if (!canvas_w || !canvas_h) {
        canvas_w = 1920;
        canvas_h = 1080;
        av_log(s, AV_LOG_WARNING, "The size of the video is unknown, "
               "assuming %dx%d.\n", canvas_w, canvas_h);
    }
    if (imsc->max_x > canvas_w || imsc->max_y > canvas_h) {
        av_log(s, AV_LOG_WARNING, "The images go beyond the %dx%d video, "
               "which is enlarged to %dx%d.\n", canvas_w, canvas_h,
               FFMAX(canvas_w, imsc->max_x), FFMAX(canvas_h, imsc->max_y));
        canvas_w = FFMAX(canvas_w, imsc->max_x);
        canvas_h = FFMAX(canvas_h, imsc->max_y);
    }

    avio_printf(pb,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<tt xmlns=\"http://www.w3.org/ns/ttml\"\n"
        "    xmlns:ttp=\"http://www.w3.org/ns/ttml#parameter\"\n"
        "    xmlns:tts=\"http://www.w3.org/ns/ttml#styling\"\n"
        "    xmlns:smpte=\"http://www.smpte-ra.org/schemas/2052-1/2010/smpte-tt\"\n");
    /* The IMSC1 Image profile has no embedded images. */
    if (!imsc->embed_images)
        avio_printf(pb, "    ttp:profile=\"http://www.w3.org/ns/ttml/profile/imsc1/image\"\n");
    avio_printf(pb, "    tts:extent=\"%dpx %dpx\"\n    xml:lang=\"", canvas_w, canvas_h);
    if (lang) {
        AVBPrint bp;
        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        av_bprint_escape(&bp, lang->value, NULL, AV_ESCAPE_MODE_XML,
                         AV_ESCAPE_FLAG_XML_DOUBLE_QUOTES);
        if (!av_bprint_is_complete(&bp)) {
            av_bprint_finalize(&bp, NULL);
            return AVERROR(ENOMEM);
        }
        avio_write(pb, bp.str, bp.len);
        av_bprint_finalize(&bp, NULL);
    }
    avio_printf(pb, "\">\n  <head>\n");

    if (imsc->embed_images) {
        avio_printf(pb, "    <metadata>\n");
        avio_write(pb, imsc->metadata.str, imsc->metadata.len);
        avio_printf(pb, "    </metadata>\n");
    }
    avio_printf(pb, "    <layout>\n");
    for (int i = 0; i < imsc->nb_regions; i++) {
        const IMSCImageRegion *r = &imsc->regions[i];
        avio_printf(pb, "      <region xml:id=\"r%d\" tts:origin=\"%dpx %dpx\" "
                    "tts:extent=\"%dpx %dpx\"/>\n", i + 1, r->x, r->y, r->w, r->h);
    }
    avio_printf(pb, "    </layout>\n  </head>\n  <body>\n    <div>\n");
    avio_write(pb, imsc->body.str, imsc->body.len);
    avio_printf(pb, "    </div>\n  </body>\n</tt>\n");

    if (!av_bprint_is_complete(&imsc->body) ||
        !av_bprint_is_complete(&imsc->metadata))
        return AVERROR(ENOMEM);
    return 0;
}

static void imsc_image_deinit(AVFormatContext *s)
{
    IMSCImageMuxContext *imsc = s->priv_data;

    for (int i = 0; i < imsc->nb_pending; i++)
        av_freep(&imsc->pending[i].png);
    av_freep(&imsc->pending);
    av_freep(&imsc->regions);
    av_freep(&imsc->image_dir);
    av_bprint_finalize(&imsc->body, NULL);
    av_bprint_finalize(&imsc->metadata, NULL);
}

#define OFFSET(x) offsetof(IMSCImageMuxContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "embed_images", "embed the images in the document in Base64 (SMPTE-TT) "
      "instead of writing PNG files next to it (IMSC1 Image)",
      OFFSET(embed_images), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, E },
    { "image_prefix", "start of the names of the PNG files, relative to the "
      "document (default: name of the document without its extension, then _)",
      OFFSET(image_prefix), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, E },
    { "last_duration", "duration of the last subtitle if nothing ends it",
      OFFSET(last_duration), AV_OPT_TYPE_DURATION, { .i64 = 5000000 }, 0, INT64_MAX, E },
    { NULL },
};

static const AVClass imsc_image_muxer_class = {
    .class_name = "imsc_image muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_imsc_image_muxer = {
    .p.name            = "imsc_image",
    .p.long_name       = NULL_IF_CONFIG_SMALL("IMSC1 Image subtitles (TTML + PNG)"),
    .p.mime_type       = "application/ttml+xml",
    .p.flags           = AVFMT_VARIABLE_FPS | AVFMT_TS_NONSTRICT,
    .p.video_codec     = AV_CODEC_ID_NONE,
    .p.audio_codec     = AV_CODEC_ID_NONE,
    .p.subtitle_codec  = AV_CODEC_ID_IMSC_IMAGE,
    .p.priv_class      = &imsc_image_muxer_class,
    .priv_data_size    = sizeof(IMSCImageMuxContext),
    .init              = imsc_image_init,
    .write_packet      = imsc_image_write_packet,
    .write_trailer     = imsc_image_write_trailer,
    .deinit            = imsc_image_deinit,
    .flags_internal    = FF_OFMT_FLAG_MAX_ONE_OF_EACH |
                         FF_OFMT_FLAG_ONLY_DEFAULT_CODECS,
};
