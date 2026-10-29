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

#ifndef IMAGE_H
#define IMAGE_H
#include <stdint.h>
#include "pdf_core.h"

void render_image_inline(Doc *d, const char *src, double *curx, double *cury, double *maxh);
int  img_get(const char *src, int width_pct, int width_px);
void render_image(Doc *d, const char *src, int width_pct, int width_px);
void free_images(void);
int  emoji_atlas_build(const char *md_text);
int  buscar_secuencia_emoji(const uint32_t *src_seq, uint8_t src_len);
void render_emoji_nativo(Doc *d, int idx, double x_pdf, double y_pdf, double size_pts);
void free_emoji_atlas(void);
#endif
