/*
 * Generate the Matroska file of the FATE chapter title tests.
 *
 * Its chapters name their titles in several ChapterDisplay elements, each in
 * the languages it states: several displays, ChapLanguageBCP47 beside
 * ChapLanguage, a display stating no language, one in "und", one naming two
 * languages and one without a string, beside a subtitle track of one cue,
 * since the demuxer reads up to the first cluster. Built here, the way apnggen
 * builds its images, so the test needs no media from the fate-suite.
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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t data[4096];
    int len;
} Buf;

static void put(Buf *b, const void *data, int n)
{
    if (b->len + n > (int)sizeof(b->data)) {
        fprintf(stderr, "mkvchaptergen: buffer overflow\n");
        exit(1);
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
}

static void put_id(Buf *b, uint32_t id)
{
    uint8_t bytes[4];
    int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;

    for (int i = 0; i < n; i++)
        bytes[i] = id >> (8 * (n - 1 - i));
    put(b, bytes, n);
}

/* Every size on eight bytes, which any EBML reader takes. */
static void put_size(Buf *b, uint64_t size)
{
    uint8_t bytes[8] = { 0x01 };

    for (int i = 1; i < 8; i++)
        bytes[i] = size >> (8 * (7 - i));
    put(b, bytes, 8);
}

static void element(Buf *b, uint32_t id, const Buf *payload)
{
    put_id(b, id);
    put_size(b, payload->len);
    put(b, payload->data, payload->len);
}

static void uint_element(Buf *b, uint32_t id, uint64_t value)
{
    Buf payload = { 0 };
    uint8_t bytes[8];

    for (int i = 0; i < 8; i++)
        bytes[i] = value >> (8 * (7 - i));
    put(&payload, bytes, 8);
    element(b, id, &payload);
}

static void string_element(Buf *b, uint32_t id, const char *value)
{
    Buf payload = { 0 };

    put(&payload, value, strlen(value));
    element(b, id, &payload);
}

enum {
    EBML              = 0x1A45DFA3,
    DOCTYPE           = 0x4282,
    DOCTYPEVERSION    = 0x4287,
    DOCTYPEREADVER    = 0x4285,
    SEGMENT           = 0x18538067,
    INFO              = 0x1549A966,
    TIMESTAMPSCALE    = 0x2AD7B1,
    TRACKS            = 0x1654AE6B,
    TRACKENTRY        = 0xAE,
    TRACKNUMBER       = 0xD7,
    TRACKUID          = 0x73C5,
    TRACKTYPE         = 0x83,
    CODECID           = 0x86,
    CLUSTER           = 0x1F43B675,
    CLUSTERTIMESTAMP  = 0xE7,
    BLOCKGROUP        = 0xA0,
    BLOCK             = 0xA1,
    BLOCKDURATION     = 0x9B,
    CHAPTERS          = 0x1043A770,
    EDITIONENTRY      = 0x45B9,
    CHAPTERATOM       = 0xB6,
    CHAPTERUID        = 0x73C4,
    CHAPTERTIMESTART  = 0x91,
    CHAPTERTIMEEND    = 0x92,
    CHAPTERDISPLAY    = 0x80,
    CHAPSTRING        = 0x85,
    CHAPLANGUAGE      = 0x437C,
    CHAPLANGUAGEBCP47 = 0x437D,
};

/* A display: its string (NULL for none), then its elements as id, value. */
static void display(Buf *atom, const char *string, int n, const uint32_t *ids,
                    const char *const *values)
{
    Buf payload = { 0 };

    if (string)
        string_element(&payload, CHAPSTRING, string);
    for (int i = 0; i < n; i++)
        string_element(&payload, ids[i], values[i]);
    element(atom, CHAPTERDISPLAY, &payload);
}

static void chapter(Buf *edition, uint64_t uid, uint64_t start_s, Buf *displays)
{
    Buf atom = { 0 };

    uint_element(&atom, CHAPTERUID, uid);
    uint_element(&atom, CHAPTERTIMESTART, start_s * 1000000000);
    uint_element(&atom, CHAPTERTIMEEND, (start_s + 10) * 1000000000);
    put(&atom, displays->data, displays->len);
    element(edition, CHAPTERATOM, &atom);
}

int main(int argc, char **argv)
{
    static const uint32_t lang[]       = { CHAPLANGUAGE };
    static const uint32_t lang2[]      = { CHAPLANGUAGE, CHAPLANGUAGE };
    static const uint32_t lang_bcp47[] = { CHAPLANGUAGE, CHAPLANGUAGEBCP47 };
    /* Track 1, timestamp 0, no flags, then the cue. */
    static const uint8_t cue[] = { 0x81, 0x00, 0x00, 0x00, 'c', 'u', 'e' };
    Buf header = { 0 }, segment = { 0 }, info = { 0 }, tracks = { 0 },
        track = { 0 }, edition = { 0 }, chapters = { 0 }, cluster = { 0 },
        group = { 0 }, block = { 0 }, displays, out = { 0 };
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: mkvchaptergen <file>\n");
        return 1;
    }

    string_element(&header, DOCTYPE, "matroska");
    uint_element(&header, DOCTYPEVERSION, 4);
    uint_element(&header, DOCTYPEREADVER, 2);
    element(&out, EBML, &header);

    uint_element(&info, TIMESTAMPSCALE, 1000000);
    element(&segment, INFO, &info);

    uint_element(&track, TRACKNUMBER, 1);
    uint_element(&track, TRACKUID, 1);
    uint_element(&track, TRACKTYPE, 0x11);
    string_element(&track, CODECID, "S_TEXT/UTF8");
    element(&tracks, TRACKENTRY, &track);
    element(&segment, TRACKS, &tracks);

    /* One display per language: the title is the last one's. */
    displays = (Buf) { 0 };
    display(&displays, "Partie 1", 1, lang, (const char *[]) { "fre" });
    display(&displays, "Part 1",   1, lang, (const char *[]) { "eng" });
    display(&displays, "Teil 1",   1, lang, (const char *[]) { "ger" });
    chapter(&edition, 1, 0, &displays);

    /* ChapLanguageBCP47 supersedes ChapLanguage; a display may name two. */
    displays = (Buf) { 0 };
    display(&displays, "Colour", 2, lang_bcp47, (const char *[]) { "eng", "en-GB" });
    display(&displays, "Colore", 2, lang2,      (const char *[]) { "ita", "lat" });
    chapter(&edition, 2, 10, &displays);

    /* No language is English; "und" is none. */
    displays = (Buf) { 0 };
    display(&displays, "Ending",   0, NULL, NULL);
    display(&displays, "Untagged", 1, lang, (const char *[]) { "und" });
    chapter(&edition, 3, 20, &displays);

    /* A display without a string names nothing and leaves the title alone. */
    displays = (Buf) { 0 };
    display(&displays, "Credits", 1, lang, (const char *[]) { "eng" });
    display(&displays, NULL,      1, lang, (const char *[]) { "fre" });
    chapter(&edition, 4, 30, &displays);

    element(&chapters, EDITIONENTRY, &edition);
    element(&segment, CHAPTERS, &chapters);

    uint_element(&cluster, CLUSTERTIMESTAMP, 0);
    put(&block, cue, sizeof(cue));
    element(&group, BLOCK, &block);
    uint_element(&group, BLOCKDURATION, 1000);
    element(&cluster, BLOCKGROUP, &group);
    element(&segment, CLUSTER, &cluster);
    element(&out, SEGMENT, &segment);

    f = fopen(argv[1], "wb");
    if (!f || fwrite(out.data, 1, out.len, f) != (size_t)out.len || fclose(f)) {
        fprintf(stderr, "mkvchaptergen: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
