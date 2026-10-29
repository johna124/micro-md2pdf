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

#ifndef PARSER_H
#define PARSER_H

#include "pdf_core.h"
#include <stdint.h>

void render_emoji_nativo(Doc *d, int idx, double x_pdf, double y_pdf, double size_pts);
int buscar_secuencia_emoji(const uint32_t* src_secuencia, uint8_t src_len);

typedef struct { int style; Buf txt; char url[1024]; } Run;
typedef struct { Run *v; int n, cap; } Runs;

typedef struct { int style; char *txt; char url[1024]; } Seg;
typedef struct { Seg *v; int n, cap; } Line2;

void parse_inline(const char *src, Runs *R);
void free_runs(Runs *R);
double segw(const char *s, size_t len, int style, double fs);
double longest_word(const char *s, double fs);
int wrap_runs(Runs *R, double maxw, double fs, Line2 **out);
void free_lines(Line2 *L, int nl);
void para_text(Doc *d, const char *txt);
void heading(Doc *d, int lvl, const char *txt);
void hr(Doc *d);
void quote_line(Doc *d, const char *txt);
void list_item(Doc *d, const char *mk, const char *txt);
void code_block(Doc *d, Buf *code);

typedef struct { int n, cap; char **c; } Row;
Row mk_row(const char *line);
int is_sep_row(const Row *r);
void free_row(Row *r);
void free_rows(Row *rows, int n);
void render_table(Doc *d, Row *rows, int nr);
char *trim(char *s);
int blank(const char *s);
int is_hr(const char *s);
int is_table_line(const char *s);
void flush_para(Doc *d, Buf *para, int *have);
int image_url(const char *t, char *url, size_t usz, int *wpct, int *wpx);

#endif
