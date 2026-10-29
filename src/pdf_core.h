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

#ifndef PDF_CORE_H
#define PDF_CORE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>

#define PW        595.28
#define PH        841.89
#define ML        48.0
#define MR        48.0
#define MT        56.0
#define MB        56.0
#define CONTENT_W (PW-ML-MR)
#define FS        10.0
#define TF        8.0
#define TPAD      4.0
#define MINW      30.0

typedef struct { char *d; size_t n, cap; } Buf;

void binit(Buf *b);
void bneed(Buf *b, size_t k);
void bput(Buf *b, const void *s, size_t k);
void bputs(Buf *b, const char *s);
void bputc(Buf *b, char c);
void bprintf(Buf *b, const char *fmt, ...);

int glyph_w(unsigned char u, int mono, int style);
double textw(const char *s, double fs, int mono);
void esc_pdf(Buf *b, const char *s);

typedef struct { Buf **pg; int n, cap; double y; } Doc;
#define CURPG(d) ((d)->pg[(d)->n-1])

void new_page(Doc *d);
void room(Doc *d, double h);

typedef struct {
    char src[1024];
    int w, h, rw, rh, cs, jpeg, flate;
    int width_pct, width_px;
    unsigned char *data;
    size_t len;
    unsigned char *zdata;
    size_t zlen;
    int smask_id;   /* -1 = sin SMask; >=0 = id de la imagen alpha */
} Img;

typedef struct { int page, img; } Use;

extern Img *imgs;
extern int nimgs, capimgs;
extern Use *uses;
extern int nuses, capuses;

void use_img(Doc *d, int id);

typedef struct {
    int page;
    double x1, y1, x2, y2;
    char url[1024];
} Link;

extern Link *links;
extern int nlinks, caplinks;

void add_link(Doc *d, double x1, double y1, double x2, double y2, const char *url);

int write_pdf(Doc *d, const char *path);

#endif
