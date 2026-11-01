/*
 * micro-md2pdf is a high-performance Markdown-to-PDF engine written in C99.
 * It compiles into a fully static ~1.4 MB binary (via musl libc) with zero
 * external runtime dependencies, enabling instant execution on any Linux
 * distribution without requiring Python, Node.js, or LaTeX
 *
 * Copyright (C) 2026  John (johna124)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Source, issues, contact: https://github.com/johna124
 */

#include "image.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <zlib.h>
#include <webp/decode.h>
#include <avif/avif.h>
#include "parser.h"
/* Altura de los badges/shields en puntos (ajustable con --height) */
#ifndef BADGE_H_PT
#define BADGE_H_PT 12.0
#endif
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


#define EMOJI_SIZE_PX 72
#define ATLAS_COLS    8
#define MAX_SEQ_DYN   16

typedef struct {
    uint32_t seq[MAX_SEQ_DYN];
    uint8_t  len;
    uint16_t idx;
} DynEmojiEntry;

static DynEmojiEntry *g_emojis     = NULL;
static int            g_emojis_n   = 0;
static int            g_emojis_cap = 0;
static int            g_atlas_id   = -2;
static int            g_atlas_alpha_id = -2;

/* ================================================================== */
/*  Utilidades de lectura de archivo y URLs                           */
/* ================================================================== */
static unsigned char *read_file_img(const char *path, size_t *outlen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    *outlen = rd;
    return buf;
}

static char *rewrite_github_url(const char *src) {
    const char *prefix = "https://github.com/";
    if (strncmp(src, prefix, strlen(prefix)) != 0) return NULL;
    const char *rest = src + strlen(prefix);
    const char *blob = strstr(rest, "/blob/");
    if (!blob) return NULL;
    size_t repo_len = (size_t)(blob - rest);
    const char *branch_start = blob + 6;
    size_t new_len = strlen("https://raw.githubusercontent.com/") + repo_len + 1 + strlen(branch_start) + 1;
    char *new_url = (char *)malloc(new_len);
    if (!new_url) return NULL;
    snprintf(new_url, new_len, "https://raw.githubusercontent.com/%.*s/%s",
             (int)repo_len, rest, branch_start);
    return new_url;
}

/* ================================================================== */
/*  Detección de formato de imagen                                    */
/* ================================================================== */
static int jpeg_info(const unsigned char *d, size_t n, int *w, int *h, int *cs) {
    if (n < 4) return 0;
    size_t i = 2;
    while (i + 9 < n) {
        if (d[i] != 0xFF) { i++; continue; }
        unsigned char m = d[i + 1];
        if (m == 0xFF) { i++; continue; }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD9)) { i += 2; continue; }
        size_t sl = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (sl < 2 || i + 2 + sl > n) break;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = ((int)d[i + 5] << 8) | d[i + 6];
            *w = ((int)d[i + 7] << 8) | d[i + 8];
            int comp = d[i + 9];
            if (comp == 1) *cs = 0;
            else if (comp == 3) *cs = 1;
            else return 0;
            return *w > 0 && *h > 0;
        }
        i += 2 + sl;
    }
    return 0;
}

static int seems_webp(const unsigned char *d, size_t n) {
    return n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0;
}

static int seems_avif(const unsigned char *d, size_t n) {
    if (n < 12) return 0;
    if (memcmp(d + 4, "ftyp", 4) != 0) return 0;
    return memcmp(d + 8, "avif", 4) == 0 ||
           memcmp(d + 8, "avis", 4) == 0 ||
           memcmp(d + 8, "mif1", 4) == 0;
}

/* ================================================================== */
/*  Decodificadores de imagen (WebP, AVIF, PNG, JPEG)                 */
/* ================================================================== */
static unsigned char *decode_webp_rgb(const unsigned char *data, size_t len, int *w, int *h) {
    int ww = 0, hh = 0;
    if (!WebPGetInfo(data, len, &ww, &hh)) return NULL;
    if (ww <= 0 || hh <= 0 || ww > 10000 || hh > 10000) return NULL;
    size_t size = (size_t)ww * (size_t)hh * 3;
    unsigned char *rgb = malloc(size);
    if (!rgb) return NULL;
    if (!WebPDecodeRGBInto(data, len, rgb, size, ww * 3)) { free(rgb); return NULL; }
    *w = ww;
    *h = hh;
    return rgb;
}

static unsigned char *decode_avif_rgb(const unsigned char *data, size_t len, int *w, int *h) {
    avifDecoder *dec = avifDecoderCreate();
    if (!dec) return NULL;
    unsigned char *rgb = NULL;
    if (avifDecoderSetIOMemory(dec, (const uint8_t *)data, len) != AVIF_RESULT_OK) goto fail;
    if (avifDecoderParse(dec) != AVIF_RESULT_OK) goto fail;
    if (avifDecoderNextImage(dec) != AVIF_RESULT_OK) goto fail;
    int ww = (int)dec->image->width;
    int hh = (int)dec->image->height;
    if (ww <= 0 || hh <= 0 || ww > 10000 || hh > 10000) goto fail;
    size_t size = (size_t)ww * (size_t)hh * 3;
    rgb = malloc(size);
    if (!rgb) goto fail;
    avifRGBImage rgbimg;
    memset(&rgbimg, 0, sizeof rgbimg);
    avifRGBImageSetDefaults(&rgbimg, dec->image);
    rgbimg.format = AVIF_RGB_FORMAT_RGB;
    rgbimg.depth = 8;
    rgbimg.pixels = rgb;
    rgbimg.rowBytes = (uint32_t)ww * 3;
    if (avifImageYUVToRGB(dec->image, &rgbimg) != AVIF_RESULT_OK) { free(rgb); rgb = NULL; goto fail; }
    *w = ww;
    *h = hh;
    avifDecoderDestroy(dec);
    return rgb;
fail:
    avifDecoderDestroy(dec);
    return rgb;
}

/* ---------- PNG decoder manual ---------- */
typedef struct { const unsigned char *p; size_t n, pos; int bit, cnt; } Bitr;

static int br_bits(Bitr *b, int k) {
    int v = 0;
    for (int i = 0; i < k; i++) {
        if (b->cnt == 0) {
            if (b->pos >= b->n) return -1;
            b->bit = b->p[b->pos++];
            b->cnt = 8;
        }
        v |= (b->bit & 1) << i;
        b->bit >>= 1;
        b->cnt--;
    }
    return v;
}

typedef struct { short cnt[16]; short sym[320]; } Huff;

static int huff_build(Huff *h, const unsigned char *len, int n) {
    for (int i = 0; i < 16; i++) h->cnt[i] = 0;
    for (int i = 0; i < n; i++) h->cnt[len[i]]++;
    long sum = 0;
    for (int i = 1; i < 16; i++) sum += (long)h->cnt[i] << (16 - i);
    if (sum > (1L << 16)) return 0;
    int offs[16];
    int k = 0;
    for (int i = 1; i < 16; i++) { offs[i] = k; k += h->cnt[i]; }
    for (int s = 0; s < n; s++)
        if (len[s]) h->sym[offs[len[s]]++] = (short)s;
    return 1;
}

static int huff_decode(Bitr *b, Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int bit = br_bits(b, 1);
        if (bit < 0) return -1;
        code |= bit;
        int count = h->cnt[len];
        if (code - first < count) return h->sym[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static int md_inflate(const unsigned char *in, size_t inlen, unsigned char **out, size_t *outlen) {
    static const int lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const int dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    static const int ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    Bitr b = {in, inlen, 0, 0, 0};
    Buf o = {0};
    int bfinal;
    do {
        bfinal = br_bits(&b, 1);
        int btype = br_bits(&b, 2);
        if (bfinal < 0 || btype < 0) goto ifail;
        if (btype == 0) {
            b.cnt = 0;
            if (b.pos + 4 > b.n) goto ifail;
            size_t len = (size_t)b.p[b.pos] | ((size_t)b.p[b.pos + 1] << 8);
            size_t nlen = (size_t)b.p[b.pos + 2] | ((size_t)b.p[b.pos + 3] << 8);
            b.pos += 4;
            if ((len & 0xFFFF) != ((~nlen) & 0xFFFF)) goto ifail;
            if (b.pos + len > b.n) goto ifail;
            bput(&o, b.p + b.pos, len);
            b.pos += len;
        } else if (btype == 1 || btype == 2) {
            Huff lh, dh;
            unsigned char ll[288], dl[30];
            if (btype == 1) {
                int i;
                for (i = 0; i < 144; i++) ll[i] = 8;
                for (; i < 256; i++) ll[i] = 9;
                for (; i < 280; i++) ll[i] = 7;
                for (; i < 288; i++) ll[i] = 8;
                for (i = 0; i < 30; i++) dl[i] = 5;
                if (!huff_build(&lh, ll, 288) || !huff_build(&dh, dl, 30)) goto ifail;
            } else {
                int hlit = br_bits(&b, 5), hdist = br_bits(&b, 5), hclen = br_bits(&b, 4);
                if (hlit < 0 || hdist < 0 || hclen < 0) goto ifail;
                hlit += 257; hdist += 1; hclen += 4;
                if (hlit > 286 || hdist > 30 || hclen > 19) goto ifail;
                unsigned char cl[19];
                memset(cl, 0, sizeof cl);
                for (int i = 0; i < hclen; i++) {
                    int v3 = br_bits(&b, 3);
                    if (v3 < 0) goto ifail;
                    cl[ord[i]] = (unsigned char)v3;
                }
                Huff ch;
                if (!huff_build(&ch, cl, 19)) goto ifail;
                unsigned char lens[320];
                memset(lens, 0, sizeof lens);
                int i = 0;
                while (i < hlit + hdist) {
                    int s = huff_decode(&b, &ch);
                    if (s < 0) goto ifail;
                    if (s < 16) { lens[i++] = (unsigned char)s; continue; }
                    int rep = 0;
                    if (s == 16) {
                        rep = br_bits(&b, 2); if (rep < 0) goto ifail; rep += 3;
                        unsigned char pv = i ? lens[i - 1] : 0;
                        while (rep-- && i < hlit + hdist) lens[i++] = pv;
                    } else if (s == 17) {
                        rep = br_bits(&b, 3); if (rep < 0) goto ifail; rep += 3;
                        while (rep-- && i < hlit + hdist) lens[i++] = 0;
                    } else {
                        rep = br_bits(&b, 7); if (rep < 0) goto ifail; rep += 11;
                        while (rep-- && i < hlit + hdist) lens[i++] = 0;
                    }
                }
                if (!huff_build(&lh, lens, hlit)) goto ifail;
                if (!huff_build(&dh, lens + hlit, hdist)) goto ifail;
            }
            for (;;) {
                int s = huff_decode(&b, &lh);
                if (s < 0) goto ifail;
                if (s == 256) break;
                if (s < 256) { char c = (char)s; bput(&o, &c, 1); continue; }
                int li = s - 257;
                if (li < 0 || li >= 29) goto ifail;
                int ex = br_bits(&b, lext[li]); if (ex < 0) goto ifail;
                size_t len = (size_t)lbase[li] + (size_t)ex;
                int di = huff_decode(&b, &dh);
                if (di < 0 || di >= 30) goto ifail;
                int exd = br_bits(&b, dext[di]); if (exd < 0) goto ifail;
                size_t dist = (size_t)dbase[di] + (size_t)exd;
                if (dist > o.n) goto ifail;
                bneed(&o, len);
                for (size_t k = 0; k < len; k++) {
                    unsigned char c = (unsigned char)o.d[o.n - dist];
                    bput(&o, &c, 1);
                }
            }
        } else goto ifail;
    } while (!bfinal);
    *out = (unsigned char *)o.d;
    *outlen = o.n;
    return 1;
ifail:
    free(o.d);
    *out = NULL;
    *outlen = 0;
    return 0;
}

/* PNG decoder — salida RGB/gris (para imágenes normales del documento) */
static unsigned char *png_decode(const unsigned char *d, size_t n,
                                 int *w, int *h, int *cs, size_t *outlen) {
    Buf ib = {0}, plte = {0};
    unsigned char *raw = NULL, *img = NULL, *out = NULL;
    int width = 0, height = 0, depth = 0, ctype = 0, ilace = 0;
    if (n < 8 || d[0] != 0x89 || d[1] != 'P') return NULL;
    size_t pos = 8;
    while (pos + 12 <= n) {
        size_t clen = ((size_t)d[pos] << 24) | ((size_t)d[pos + 1] << 16) |
                      ((size_t)d[pos + 2] << 8) | d[pos + 3];
        const unsigned char *cd = d + pos + 8;
        if (pos + 12 + clen > n) break;
        if (!memcmp(d + pos + 4, "IHDR", 4) && clen >= 13) {
            width = (cd[0] << 24) | (cd[1] << 16) | (cd[2] << 8) | cd[3];
            height = (cd[4] << 24) | (cd[5] << 16) | (cd[6] << 8) | cd[7];
            depth = cd[8]; ctype = cd[9]; ilace = cd[12];
        } else if (!memcmp(d + pos + 4, "PLTE", 4)) bput(&plte, cd, clen);
        else if (!memcmp(d + pos + 4, "IDAT", 4)) bput(&ib, cd, clen);
        else if (!memcmp(d + pos + 4, "IEND", 4)) break;
        pos += 12 + clen;
    }
    if (!width || !height || !ib.n || ilace || depth != 8) goto pngfail;
    if (ctype != 0 && ctype != 2 && ctype != 3 && ctype != 6) goto pngfail;
    if (width > 6000 || height > 6000) goto pngfail;
    unsigned char *zdata = ib.d;
    size_t zlen = ib.n;
    if (zlen > 2 && (zdata[0] & 0x0F) == 8) {
        size_t off = (zdata[1] & 0x20) ? 6 : 2;
        if (zlen > off) { zdata += off; zlen -= off; }
    }
    size_t rawlen = 0;
    if (!md_inflate(zdata, zlen, &raw, &rawlen)) goto pngfail;
    int chans = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : 4;
    size_t stride = (size_t)width * chans;
    if (rawlen < (stride + 1) * (size_t)height) goto pngfail;
    img = (unsigned char *)malloc(stride * (size_t)height);
    if (!img) goto pngfail;
    for (int y = 0; y < height; y++) {
        const unsigned char *line = raw + y * (stride + 1);
        unsigned char fl = line[0];
        if (fl > 4) goto pngfail;
        const unsigned char *src = line + 1;
        unsigned char *dst = img + (size_t)y * stride;
        const unsigned char *prev = y ? img + (size_t)(y - 1) * stride : NULL;
        for (size_t x = 0; x < stride; x++) {
            unsigned char a = (x >= (size_t)chans) ? dst[x - chans] : 0;
            unsigned char bb = prev ? prev[x] : 0;
            unsigned char c = (prev && x >= (size_t)chans) ? prev[x - chans] : 0;
            int v = src[x];
            switch (fl) {
                case 1: v += a; break;
                case 2: v += bb; break;
                case 3: v += (a + bb) / 2; break;
                case 4: {
                    int p = a + bb - c;
                    int pa = abs(p - a), pb = abs(p - bb), pc = abs(p - c);
                    v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? bb : c);
                } break;
            }
            dst[x] = (unsigned char)v;
        }
    }
    if (ctype == 0) {
        out = (unsigned char *)malloc((size_t)width * height);
        if (!out) goto pngfail;
        memcpy(out, img, (size_t)width * height);
        *cs = 0;
        *outlen = (size_t)width * height;
    } else {
        size_t np = (size_t)width * height;
        out = (unsigned char *)malloc(np * 3);
        if (!out) goto pngfail;
        if (ctype == 2) memcpy(out, img, np * 3);
        else if (ctype == 6) {
            for (size_t i = 0; i < np; i++) {
                out[i * 3] = img[i * 4];
                out[i * 3 + 1] = img[i * 4 + 1];
                out[i * 3 + 2] = img[i * 4 + 2];
            }
        } else {
            if (plte.n < 3) goto pngfail;
            for (size_t i = 0; i < np; i++) {
                int idx = img[i] * 3;
                if ((size_t)idx + 2 >= plte.n) idx = 0;
                out[i * 3] = plte.d[idx];
                out[i * 3 + 1] = plte.d[idx + 1];
                out[i * 3 + 2] = plte.d[idx + 2];
            }
        }
        *cs = 1;
        *outlen = np * 3;
    }
    *w = width;
    *h = height;
    free(raw); free(img); free(ib.d); free(plte.d);
    return out;
pngfail:
    free(raw); free(img); free(out); free(ib.d); free(plte.d);
    return NULL;
}

/* PNG decoder — salida RGBA (para el atlas de emojis, preserva alpha) */
static unsigned char *png_decode_rgba(const unsigned char *d, size_t n, int *w, int *h) {
    Buf ib = {0}, plte = {0};
    unsigned char *raw = NULL, *img = NULL, *out = NULL;
    int width = 0, height = 0, depth = 0, ctype = 0, ilace = 0;
    if (n < 8 || d[0] != 0x89 || d[1] != 'P') return NULL;
    size_t pos = 8;
    while (pos + 12 <= n) {
        size_t clen = ((size_t)d[pos] << 24) | ((size_t)d[pos + 1] << 16) |
                      ((size_t)d[pos + 2] << 8) | d[pos + 3];
        const unsigned char *cd = d + pos + 8;
        if (pos + 12 + clen > n) break;
        if (!memcmp(d + pos + 4, "IHDR", 4) && clen >= 13) {
            width = (cd[0] << 24) | (cd[1] << 16) | (cd[2] << 8) | cd[3];
            height = (cd[4] << 24) | (cd[5] << 16) | (cd[6] << 8) | cd[7];
            depth = cd[8]; ctype = cd[9]; ilace = cd[12];
        } else if (!memcmp(d + pos + 4, "PLTE", 4)) bput(&plte, cd, clen);
        else if (!memcmp(d + pos + 4, "IDAT", 4)) bput(&ib, cd, clen);
        else if (!memcmp(d + pos + 4, "IEND", 4)) break;
        pos += 12 + clen;
    }
    if (!width || !height || !ib.n || ilace || depth != 8) goto rfail;
    if (ctype != 0 && ctype != 2 && ctype != 3 && ctype != 6) goto rfail;
    if (width > 6000 || height > 6000) goto rfail;
    unsigned char *zdata = ib.d;
    size_t zlen = ib.n;
    if (zlen > 2 && (zdata[0] & 0x0F) == 8) {
        size_t off = (zdata[1] & 0x20) ? 6 : 2;
        if (zlen > off) { zdata += off; zlen -= off; }
    }
    size_t rawlen = 0;
    if (!md_inflate(zdata, zlen, &raw, &rawlen)) goto rfail;
    int chans = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : 4;
    size_t stride = (size_t)width * chans;
    if (rawlen < (stride + 1) * (size_t)height) goto rfail;
    img = (unsigned char *)malloc(stride * (size_t)height);
    if (!img) goto rfail;
    for (int y = 0; y < height; y++) {
        const unsigned char *line = raw + y * (stride + 1);
        unsigned char fl = line[0];
        if (fl > 4) goto rfail;
        const unsigned char *src = line + 1;
        unsigned char *dst = img + (size_t)y * stride;
        const unsigned char *prev = y ? img + (size_t)(y - 1) * stride : NULL;
        for (size_t x = 0; x < stride; x++) {
            unsigned char a = (x >= (size_t)chans) ? dst[x - chans] : 0;
            unsigned char bb = prev ? prev[x] : 0;
            unsigned char c = (prev && x >= (size_t)chans) ? prev[x - chans] : 0;
            int v = src[x];
            switch (fl) {
                case 1: v += a; break;
                case 2: v += bb; break;
                case 3: v += (a + bb) / 2; break;
                case 4: {
                    int p = a + bb - c;
                    int pa = abs(p - a), pb = abs(p - bb), pc = abs(p - c);
                    v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? bb : c);
                } break;
            }
            dst[x] = (unsigned char)v;
        }
    }
    size_t np = (size_t)width * height;
    out = (unsigned char *)malloc(np * 4);
    if (!out) goto rfail;
    if (ctype == 6) {
        memcpy(out, img, np * 4);
    } else if (ctype == 2) {
        for (size_t i = 0; i < np; i++) {
            out[i * 4] = img[i * 3];
            out[i * 4 + 1] = img[i * 3 + 1];
            out[i * 4 + 2] = img[i * 3 + 2];
            out[i * 4 + 3] = 255;
        }
    } else if (ctype == 0) {
        for (size_t i = 0; i < np; i++) {
            out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = img[i];
            out[i * 4 + 3] = 255;
        }
    } else {
        if (plte.n < 3) goto rfail;
        for (size_t i = 0; i < np; i++) {
            int idx = img[i] * 3;
            if ((size_t)idx + 2 >= plte.n) idx = 0;
            out[i * 4] = plte.d[idx];
            out[i * 4 + 1] = plte.d[idx + 1];
            out[i * 4 + 2] = plte.d[idx + 2];
            out[i * 4 + 3] = 255;
        }
    }
    *w = width;
    *h = height;
    free(raw); free(img); free(ib.d); free(plte.d);
    return out;
rfail:
    free(raw); free(img); free(out); free(ib.d); free(plte.d);
    return NULL;
}

/* ================================================================== */
/*  Resize, Flate, JPEG encode/decode                                 */
/* ================================================================== */
static unsigned char *resize_rgb(const unsigned char *src, int sw, int sh,
                                 int dw, int dh, int cs) {
    int chans = (cs == 0) ? 1 : 3;
    unsigned char *dst = (unsigned char *)malloc((size_t)dw * (size_t)dh * chans);
    if (!dst) return NULL;
    for (int y = 0; y < dh; y++) {
        double sy = (double)y * (double)sh / (double)dh;
        int y0 = (int)sy;
        int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        double fy = sy - y0;
        for (int x = 0; x < dw; x++) {
            double sx = (double)x * (double)sw / (double)dw;
            int x0 = (int)sx;
            int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            double fx = sx - x0;
            for (int c = 0; c < chans; c++) {
                double v00 = src[(y0 * sw + x0) * chans + c];
                double v01 = src[(y0 * sw + x1) * chans + c];
                double v10 = src[(y1 * sw + x0) * chans + c];
                double v11 = src[(y1 * sw + x1) * chans + c];
                double v = v00 * (1.0 - fx) * (1.0 - fy)
                         + v01 * fx * (1.0 - fy)
                         + v10 * (1.0 - fx) * fy
                         + v11 * fx * fy;
                int iv = (int)(v + 0.5);
                if (iv < 0) iv = 0;
                if (iv > 255) iv = 255;
                dst[(y * dw + x) * chans + c] = (unsigned char)iv;
            }
        }
    }
    return dst;
}

static unsigned char *flate_compress(const unsigned char *data, size_t len, size_t *outlen) {
    uLongf destLen = compressBound(len);
    unsigned char *dest = malloc(destLen);
    if (!dest) return NULL;
    if (compress2(dest, &destLen, data, len, 9) != Z_OK) { free(dest); return NULL; }
    *outlen = (size_t)destLen;
    return dest;
}

static void jpeg_buf_write(void *context, void *data, int size) {
    if (size > 0) bput((Buf *)context, data, (size_t)size);
}

static unsigned char *encode_jpeg_rgb(const unsigned char *rgb, int w, int h, size_t *outlen) {
    Buf b = {0};
    if (!stbi_write_jpg_to_func(jpeg_buf_write, &b, w, h, 3, rgb, 85)) {
        free(b.d);
        *outlen = 0;
        return NULL;
    }
    if (b.n == 0) { free(b.d); *outlen = 0; return NULL; }
    *outlen = b.n;
    return (unsigned char *)b.d;
}

static unsigned char *decode_jpeg_rgb(const unsigned char *data, size_t len, int *w, int *h) {
    int x = 0, y = 0, comp = 0;
    unsigned char *tmp = stbi_load_from_memory(data, (int)len, &x, &y, &comp, 3);
    if (!tmp) return NULL;
    if (x <= 0 || y <= 0 || x > 10000 || y > 10000) { stbi_image_free(tmp); return NULL; }
    size_t sz = (size_t)x * (size_t)y * 3;
    unsigned char *rgb = malloc(sz);
    if (!rgb) { stbi_image_free(tmp); return NULL; }
    memcpy(rgb, tmp, sz);
    stbi_image_free(tmp);
    *w = x;
    *h = y;
    return rgb;
}

/* ================================================================== */
/*  Registro de imágenes en el documento                              */
/* ================================================================== */
static void target_dims(int ow, int oh, int wpct, int wpx, int *tw, int *th) {
    if (ow <= 0 || oh <= 0) { *tw = ow; *th = oh; return; }
    double content_px = CONTENT_W * 96.0 / 72.0;
    int target = ow;
    if (wpct > 0) target = (int)(content_px * (double)wpct / 100.0 + 0.5);
    else if (wpx > 0) target = wpx;
    else target = ow;
    if (target < 1) target = 1;
    if (target > ow) target = ow;
    *tw = target;
    *th = (int)((double)oh * (double)target / (double)ow + 0.5);
    if (*th < 1) *th = 1;
}

static int fill_jpeg_target(Img *tmp, unsigned char *rgb, int ow, int oh, int tw, int th) {
    if (tw != ow || th != oh) {
        unsigned char *r = resize_rgb(rgb, ow, oh, tw, th, 1);
        free(rgb);
        if (!r) return -1;
        rgb = r;
    }
    size_t jl = 0;
    unsigned char *jpg = encode_jpeg_rgb(rgb, tw, th, &jl);
    free(rgb);
    if (!jpg) return -1;
    tmp->w = tw; tmp->h = th;
    tmp->rw = tw; tmp->rh = th;
    tmp->cs = 1; tmp->jpeg = 1; tmp->flate = 0;
    tmp->data = jpg; tmp->len = jl;
    tmp->zdata = NULL; tmp->zlen = 0;
    return 0;
}

static int fill_flate_target(Img *tmp, unsigned char *rgb, size_t ol,
                             int ow, int oh, int cs, int tw, int th) {
    int chans = (cs == 0) ? 1 : 3;
    if (tw != ow || th != oh) {
        unsigned char *r = resize_rgb(rgb, ow, oh, tw, th, cs);
        free(rgb);
        if (!r) return -1;
        rgb = r;
        ol = (size_t)tw * (size_t)th * chans;
    }
    size_t zlen = 0;
    unsigned char *z = flate_compress(rgb, ol, &zlen);
    if (!z) {
        tmp->data = rgb; tmp->len = ol; tmp->flate = 0;
    } else {
        free(rgb);
        tmp->data = NULL; tmp->len = 0;
        tmp->zdata = z; tmp->zlen = zlen; tmp->flate = 1;
    }
    tmp->w = tw; tmp->h = th;
    tmp->rw = tw; tmp->rh = th;
    tmp->cs = cs; tmp->jpeg = 0;
    return 0;
}

static int register_img(Img *tmp) {
    if (nimgs == capimgs) {
        int nc = capimgs ? capimgs * 2 : 8;
        Img *ni = realloc(imgs, (size_t)nc * sizeof *ni);
        if (!ni) return -1;
        imgs = ni;
        capimgs = nc;
    }
    imgs[nimgs] = *tmp;
    return nimgs++;
}

/* ================================================================== */
/*  img_get — descarga, decodifica y registra una imagen externa      */
/* ================================================================== */
int img_get(const char *src, int wpct, int wpx) {
    if (!src || !*src) return -1;
    for (int i = 0; i < nimgs; i++) {
        if (strncmp(imgs[i].src, src, sizeof imgs[i].src - 1) == 0 &&
            imgs[i].width_pct == wpct && imgs[i].width_px == wpx)
            return i;
    }
    char *actual_src = rewrite_github_url(src);
    int need_free = (actual_src != NULL);
    if (!actual_src) actual_src = (char *)src;

/* MAGIA: Forzar formato PNG para shields.io en cualquier caso */
char shields_png[2048];
if (strstr(actual_src, "img.shields.io")) {
    char *svg_ext = strstr(actual_src, ".svg");
    char *query_param = strchr(actual_src, '?');

    if (svg_ext && (!query_param || svg_ext < query_param)) {
        snprintf(shields_png, sizeof shields_png, "%s", actual_src);
        char *ext_in_buf = strstr(shields_png, ".svg");
        if (ext_in_buf) {
            memcpy(ext_in_buf, ".png", 4);
        }
    } else {
        if (query_param) {
            size_t path_len = query_param - actual_src;
            snprintf(shields_png, sizeof shields_png, "%.*s.png%s", 
                     (int)path_len, actual_src, query_param);
        } else {
            snprintf(shields_png, sizeof shields_png, "%s.png", actual_src);
        }
    }
    if (need_free) { 
        free(actual_src); 
        need_free = 0; 
    }
    actual_src = shields_png;
}


    unsigned char *data = NULL;
    size_t len = 0;
    if (strncmp(actual_src, "http://", 7) == 0 || strncmp(actual_src, "https://", 8) == 0)
        data = http_get(actual_src, &len);
    else
        data = read_file_img(actual_src, &len);
    if (need_free) free(actual_src);
    if (!data || len == 0) { free(data); return -1; }
    Img tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.smask_id = -1;
    strncpy(tmp.src, src, sizeof tmp.src - 1);
    tmp.width_pct = wpct;
    tmp.width_px = wpx;
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        int w = 0, h = 0, cs = 1;
        if (!jpeg_info(data, len, &w, &h, &cs)) { free(data); return -1; }
        int tw = 0, th = 0;
        target_dims(w, h, wpct, wpx, &tw, &th);
        if (tw == w && th == h) {
            tmp.w = w; tmp.h = h; tmp.rw = w; tmp.rh = h;
            tmp.cs = cs; tmp.jpeg = 1; tmp.flate = 0;
            tmp.data = data; tmp.len = len;
            int id = register_img(&tmp);
            if (id >= 0) return id;
            free(data);
            return -1;
        }
        unsigned char *rgb = decode_jpeg_rgb(data, len, &w, &h);
        free(data);
        if (!rgb) return -1;
        if (fill_jpeg_target(&tmp, rgb, w, h, tw, th) != 0) return -1;
        int id = register_img(&tmp);
        if (id >= 0) return id;
        free(tmp.data);
        return -1;
    }
    if (len >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        int w = 0, h = 0, cs = 1;
        size_t ol = 0;
        unsigned char *rgb = png_decode(data, len, &w, &h, &cs, &ol);
        free(data);
        if (!rgb) return -1;
        int tw = 0, th = 0;
        target_dims(w, h, wpct, wpx, &tw, &th);
        if (fill_flate_target(&tmp, rgb, ol, w, h, cs, tw, th) != 0) return -1;
        int id = register_img(&tmp);
        if (id >= 0) return id;
        if (tmp.data) free(tmp.data);
        if (tmp.zdata) free(tmp.zdata);
        return -1;
    }
    if (seems_webp(data, len)) {
        int w = 0, h = 0;
        unsigned char *rgb = decode_webp_rgb(data, len, &w, &h);
        free(data);
        if (!rgb) return -1;
        int tw = 0, th = 0;
        target_dims(w, h, wpct, wpx, &tw, &th);
        if (fill_jpeg_target(&tmp, rgb, w, h, tw, th) != 0) return -1;
        int id = register_img(&tmp);
        if (id >= 0) return id;
        free(tmp.data);
        return -1;
    }
    if (seems_avif(data, len)) {
        int w = 0, h = 0;
        unsigned char *rgb = decode_avif_rgb(data, len, &w, &h);
        free(data);
        if (!rgb) return -1;
        int tw = 0, th = 0;
        target_dims(w, h, wpct, wpx, &tw, &th);
        if (fill_jpeg_target(&tmp, rgb, w, h, tw, th) != 0) return -1;
        int id = register_img(&tmp);
        if (id >= 0) return id;
        free(tmp.data);
        return -1;
    }
    free(data);
    return -1;
    free(data);
    return -1;
}

/* ================================================================== */
/*  render_image — dibuja una imagen externa en el PDF                */
/* ================================================================== */
void render_image(Doc *d, const char *src, int wpct, int wpx) {
    int id = img_get(src, wpct, wpx);
    if (id < 0) {
        para_text(d, "[image not available]");
        return;
    }
    Img *im = &imgs[id];
    if (im->rw <= 0 || im->rh <= 0) return;
    if (!im->data && !im->zdata) return;
    double wp = ((double)im->rw / 96.0) * 72.0;
    double hp = ((double)im->rh / 96.0) * 72.0;
    if (wp > CONTENT_W) {
        double sc = CONTENT_W / wp;
        wp *= sc; hp *= sc;
    }
    double maxh = PH - MT - MB;
    if (hp > maxh) {
        double sc = maxh / hp;
        wp *= sc; hp *= sc;
    }
    room(d, hp + 10.0);
    use_img(d, id);
    double x = ML + (CONTENT_W - wp) / 2.0;
    double y = d->y - hp;
    bprintf(CURPG(d), "q %.2f 0 0 %.2f %.2f %.2f cm /Im%d Do Q\n", wp, hp, x, y, id);
    d->y -= hp + 10.0;
}

/* ================================================================== */
/*  ATLAS DINÁMICO DE EMOJIS                                          */
/* ================================================================== */
/* Helper: decodifica un codepoint UTF-8 */
static int read_utf8_cp(const char *s, size_t n, uint32_t *cp) {
    if (n == 0) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && n >= 2) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && n >= 3) {
        *cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && n >= 4) {
        *cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

/* Determina si un codepoint pertenece a un rango de emoji */
static int is_emoji_codepoint(uint32_t cp) {
    if (cp >= 0x1F600 && cp <= 0x1F64F) return 1;
    if (cp >= 0x1F300 && cp <= 0x1F5FF) return 1;
    if (cp >= 0x1F680 && cp <= 0x1F6FF) return 1;
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return 1;
    if (cp >= 0x1F900 && cp <= 0x1F9FF) return 1;
    if (cp >= 0x1FA70 && cp <= 0x1FAFF) return 1;
    if (cp >= 0x2600 && cp <= 0x26FF) return 1;
    if (cp >= 0x2700 && cp <= 0x27BF) return 1;
    if (cp >= 0x2300 && cp <= 0x23FF) return 1;
    if (cp == 0xFE0F) return 1;
    if (cp == 0x200D) return 1;
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return 1;
    if (cp >= 0x1F170 && cp <= 0x1F251) return 1;
    if (cp == 0x2B50 || cp == 0x2B55 || cp == 0x2B1B || cp == 0x2B1C) return 1;
    if (cp == 0x1F004 || cp == 0x1F0CF) return 1;
    if (cp == 0x3030 || cp == 0x303D) return 1;
    if (cp == 0x2049 || cp == 0x203C) return 1;
    if (cp == 0x00A9 || cp == 0x00AE) return 1;
    if (cp == 0x2122 || cp == 0x2139) return 1;
    if (cp >= 0x2194 && cp <= 0x21AA) return 1;
    if (cp >= 0x25AA && cp <= 0x25FE) return 1;
    if (cp == 0x2934 || cp == 0x2935) return 1;
    if (cp == 0x3297 || cp == 0x3299) return 1;
    return 0;
}

/* Determina si un codepoint es un modificador que se agrupa al emoji previo */
static int is_emoji_modifier(uint32_t cp) {
    if (cp == 0xFE0F) return 1;
    if (cp == 0x200D) return 1;
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return 1;
    return 0;
}

/* Añade una secuencia de emojis al diccionario dinámico si no existe ya */
static void add_emoji_seq(const uint32_t *seq, int len) {
    for (int i = 0; i < g_emojis_n; i++) {
        if (g_emojis[i].len == len && memcmp(g_emojis[i].seq, seq, len * sizeof(uint32_t)) == 0)
            return;
    }
    if (g_emojis_n == g_emojis_cap) {
        int nc = g_emojis_cap ? g_emojis_cap * 2 : 16;
        DynEmojiEntry *ne = realloc(g_emojis, nc * sizeof(DynEmojiEntry));
        if (!ne) return;
        g_emojis = ne;
        g_emojis_cap = nc;
    }
    DynEmojiEntry *e = &g_emojis[g_emojis_n];
    memset(e->seq, 0, sizeof(e->seq));
    for (int i = 0; i < len && i < MAX_SEQ_DYN; i++) e->seq[i] = seq[i];
    e->len = len;
    e->idx = g_emojis_n;
    g_emojis_n++;
}

/* Escanea el texto Markdown completo y extrae todas las secuencias de emojis */
static void scan_emojis(const char *text) {
    size_t n = strlen(text);
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0;
        int len = read_utf8_cp(text + i, n - i, &cp);
        if (len == 0) break;
        if (is_emoji_codepoint(cp) && !is_emoji_modifier(cp)) {
            uint32_t seq[MAX_SEQ_DYN];
            int seq_len = 0;
            seq[seq_len++] = cp;
            size_t j = i + len;
            while (j < n && seq_len < MAX_SEQ_DYN) {
                uint32_t next_cp = 0;
                int next_len = read_utf8_cp(text + j, n - j, &next_cp);
                if (next_len == 0) break;
                if (is_emoji_modifier(next_cp)) {
                    seq[seq_len++] = next_cp;
                    j += next_len;
                    if (next_cp == 0x200D && j < n && seq_len < MAX_SEQ_DYN) {
                        uint32_t after_zwj = 0;
                        int after_len = read_utf8_cp(text + j, n - j, &after_zwj);
                        if (after_len > 0 && is_emoji_codepoint(after_zwj)) {
                            seq[seq_len++] = after_zwj;
                            j += after_len;
                        }
                    }
                } else {
                    break;
                }
            }
            add_emoji_seq(seq, seq_len);
            i = j;
        } else {
            i += len;
        }
    }
}

/* Construye la URL de descarga del PNG de Twemoji para una secuencia dada */
static void build_twemoji_url(const uint32_t *seq, int len, char *url, size_t url_sz, int keep_fe0f) {
    const char *base = "https://cdnjs.cloudflare.com/ajax/libs/twemoji/14.0.2/72x72/";
    size_t off = snprintf(url, url_sz, "%s", base);
    int first = 1;
    for (int i = 0; i < len; i++) {
        if (seq[i] == 0xFE0F && !keep_fe0f) continue;
        if (!first) off += snprintf(url + off, url_sz - off, "-");
        off += snprintf(url + off, url_sz - off, "%x", seq[i]);
        first = 0;
    }
    snprintf(url + off, url_sz - off, ".png");
}

/* Función principal: construye el atlas dinámico desde el texto Markdown */
int emoji_atlas_build(const char *md_text) {
    if (!md_text) return 0;
    if (g_emojis) { free(g_emojis); g_emojis = NULL; }
    g_emojis_n = 0;
    g_emojis_cap = 0;
    g_atlas_id = -2;
    g_atlas_alpha_id = -2;
    scan_emojis(md_text);
    fprintf(stderr, "[EMOJI] %d unique sequences detected in the MD\n", g_emojis_n);
    for (int dbg = 0; dbg < g_emojis_n; dbg++) {
        fprintf(stderr, "  [%d] len=%d seq=", dbg, g_emojis[dbg].len);
        for (int k = 0; k < g_emojis[dbg].len; k++)
            fprintf(stderr, "U+%04X ", g_emojis[dbg].seq[k]);
        fprintf(stderr, "\n");
    }
    if (g_emojis_n == 0) return 0;
    int cols = ATLAS_COLS;
    int rows = (g_emojis_n + cols - 1) / cols;
    int atlas_w = cols * EMOJI_SIZE_PX;
    int atlas_h = rows * EMOJI_SIZE_PX;
    size_t atlas_sz = (size_t)atlas_w * atlas_h * 4;
    unsigned char *atlas_rgba = calloc(1, atlas_sz);
    if (!atlas_rgba) return 0;
    int descargados = 0;
    for (int i = 0; i < g_emojis_n; i++) {
        char url[512];
        size_t png_len = 0;
        unsigned char *png_data = NULL;
        build_twemoji_url(g_emojis[i].seq, g_emojis[i].len, url, sizeof url, 0);
        png_data = http_get(url, &png_len);
        if (!png_data) {
            build_twemoji_url(g_emojis[i].seq, g_emojis[i].len, url, sizeof url, 1);
            png_data = http_get(url, &png_len);
        }
        if (!png_data) continue;
        int w = 0, h = 0;
        unsigned char *rgba = png_decode_rgba(png_data, png_len, &w, &h);
        free(png_data);
        if (!rgba || w != EMOJI_SIZE_PX || h != EMOJI_SIZE_PX) {
            if (rgba) free(rgba);
            continue;
        }
        int col = i % cols;
        int row = i / cols;
        int x0 = col * EMOJI_SIZE_PX;
        int y0 = row * EMOJI_SIZE_PX;
        for (int y = 0; y < EMOJI_SIZE_PX; y++) {
            for (int x = 0; x < EMOJI_SIZE_PX; x++) {
                int src_idx = (y * EMOJI_SIZE_PX + x) * 4;
                int dst_idx = ((y0 + y) * atlas_w + (x0 + x)) * 4;
                atlas_rgba[dst_idx + 0] = rgba[src_idx + 0];
                atlas_rgba[dst_idx + 1] = rgba[src_idx + 1];
                atlas_rgba[dst_idx + 2] = rgba[src_idx + 2];
                atlas_rgba[dst_idx + 3] = rgba[src_idx + 3];
            }
        }
        free(rgba);
        descargados++;
    }
    fprintf(stderr, "[EMOJI] %d/%d emojis downloaded from Twemoji\n", descargados, g_emojis_n);
    if (descargados == 0) {
        free(atlas_rgba);
        free(g_emojis);
        g_emojis = NULL;
        g_emojis_n = 0;
        g_emojis_cap = 0;
        return 0;
    }
    size_t np = (size_t)atlas_w * atlas_h;
    unsigned char *atlas_rgb = malloc(np * 3);
    unsigned char *atlas_alpha = malloc(np);
    if (!atlas_rgb || !atlas_alpha) {
        free(atlas_rgba);
        if (atlas_rgb) free(atlas_rgb);
        if (atlas_alpha) free(atlas_alpha);
        return 0;
    }
 
    for (size_t i = 0; i < np; i++) {
        unsigned char r = atlas_rgba[i * 4 + 0];
        unsigned char g = atlas_rgba[i * 4 + 1];
        unsigned char b = atlas_rgba[i * 4 + 2];
        unsigned char a = atlas_rgba[i * 4 + 3];
        atlas_rgb[i * 3 + 0] = r;
        atlas_rgb[i * 3 + 1] = g;
        atlas_rgb[i * 3 + 2] = b;
        atlas_alpha[i]       = a;
    }
    free(atlas_rgba);
    Img tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.smask_id = -1;
    strncpy(tmp.src, "EMOJI_ATLAS", sizeof tmp.src - 1);
    size_t ol = np * 3;
    if (fill_flate_target(&tmp, atlas_rgb, ol, atlas_w, atlas_h, 1, atlas_w, atlas_h) != 0)
        return 0;
    int id = register_img(&tmp);
    if (id < 0) {
        if (tmp.data) free(tmp.data);
        if (tmp.zdata) free(tmp.zdata);
        return 0;
    }
    g_atlas_id = id;
    Img tmp_a;
    memset(&tmp_a, 0, sizeof tmp_a);
    tmp_a.smask_id = -1;
    strncpy(tmp_a.src, "EMOJI_ALPHA", sizeof tmp_a.src - 1);
    if (fill_flate_target(&tmp_a, atlas_alpha, np, atlas_w, atlas_h, 0, atlas_w, atlas_h) != 0)
        return 0;
    int alpha_id = register_img(&tmp_a);
    if (alpha_id < 0) {
        if (tmp_a.data) free(tmp_a.data);
        if (tmp_a.zdata) free(tmp_a.zdata);
        return 0;
    }
    g_atlas_alpha_id = alpha_id;
    imgs[id].smask_id = alpha_id;
    return g_emojis_n;
}

/* Búsqueda de una secuencia de emojis en el diccionario dinámico */
int buscar_secuencia_emoji(const uint32_t *src_seq, uint8_t src_len) {
    if (src_len == 0 || src_len > MAX_SEQ_DYN) return -1;
    fprintf(stderr, "[EMOJI-SEARCH] Searchin len=%d seq=", src_len);
    for (int k = 0; k < src_len; k++) fprintf(stderr, "U+%04X ", src_seq[k]);
    fprintf(stderr, "\n");
    for (int i = 0; i < g_emojis_n; i++) {
        if (g_emojis[i].len != src_len) continue;
        int match = 1;
        for (int k = 0; k < src_len; k++) {
            if (g_emojis[i].seq[k] != src_seq[k]) { match = 0; break; }
        }
        if (match) {
            fprintf(stderr, "[EMOJI-SEARCH] FOUND! idx=%d\n", g_emojis[i].idx);
            return g_emojis[i].idx;
        }
    }
    fprintf(stderr, "[EMOJI-SEARCH] NOT found\n");
    return -1;
}

/* Renderizado de un emoji desde el atlas */
void render_emoji_nativo(Doc *d, int idx, double x_pdf, double y_pdf, double size_pts) {
    if (g_atlas_id < 0 || idx < 0 || idx >= g_emojis_n) return;
    use_img(d, g_atlas_id);
    int cols = ATLAS_COLS;
    int rows = (g_emojis_n + cols - 1) / cols;
    int col = idx % cols;
    int row = idx / cols;
    double total_cols = cols;
    double total_rows = rows;
    double factor_w = size_pts * total_cols;
    double factor_h = size_pts * total_rows;
    double offset_x = x_pdf - (col * size_pts);
    double offset_y = y_pdf - ((total_rows - 1 - row) * size_pts);
    Buf *pg = CURPG(d);
    bprintf(pg, "q\n");
    bprintf(pg, "%.2f %.2f %.2f %.2f re W n\n", x_pdf, y_pdf, size_pts, size_pts);
    bprintf(pg, "%.2f 0 0 %.2f %.2f %.2f cm /Im%d Do\n",
            factor_w, factor_h, offset_x, offset_y, g_atlas_id);
    bprintf(pg, "Q\n");
}

/* Limpieza del atlas dinámico */
void free_emoji_atlas(void) {
    if (g_emojis) {
        free(g_emojis);
        g_emojis = NULL;
    }
    g_emojis_n = 0;
    g_emojis_cap = 0;
    g_atlas_id = -2;
    g_atlas_alpha_id = -2;
}

/* ================================================================== */
/*  Liberación global de recursos                                     */
/* ================================================================== */
void free_images(void) {
    for (int i = 0; i < nimgs; i++) {
        if (imgs[i].data) free(imgs[i].data);
        if (imgs[i].zdata) free(imgs[i].zdata);
    }
    free(imgs); imgs = NULL; nimgs = 0; capimgs = 0;
    free(uses); uses = NULL; nuses = 0; capuses = 0;
    free(links); links = NULL; nlinks = 0; caplinks = 0;
    free_emoji_atlas();
}

void render_image_inline(Doc *d, const char *src, double *curx, double *cury, double *maxh) {
    int id = img_get(src, 0, 0);
    if (id < 0) return;
    Img *im = &imgs[id];
    if (im->rw <= 0 || im->rh <= 0) return;

    double wp = ((double)im->rw / 96.0) * 72.0;
    double hp = ((double)im->rh / 96.0) * 72.0;
    
    /* Tamaño exacto de GitHub (20px = 15pt de alto) */
    if (hp > BADGE_H_PT) {
        double sc = BADGE_H_PT / hp;
        wp *= sc;
        hp = BADGE_H_PT;
    }

    if (wp > CONTENT_W) { 
        double sc = CONTENT_W / wp; 
        wp *= sc; hp *= sc; 
    }

    /* Si no cabe en la fila actual, saltar a la siguiente */
    if (*curx + wp > ML + CONTENT_W) {
        *curx = ML;
        *cury -= *maxh + 6.0;
        *maxh = 0;
    }

    /* Comprobar si hace falta nueva página */
    double old_y = d->y;
    room(d, hp + 10.0);
    if (d->y != old_y) {
        *curx = ML;
        *cury = d->y;
        *maxh = 0;
    }

    use_img(d, id);
    bprintf(CURPG(d), "q %.2f 0 0 %.2f %.2f %.2f cm /Im%d Do Q\n", 
            wp, hp, *curx, *cury - hp, id);

    *curx += wp + 4.0; /* Separación de 4pt entre badges */
    if (hp > *maxh) *maxh = hp;
}
