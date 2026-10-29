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

#include "pdf_core.h"
#include "parser.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_utf8 = 1;

/* ================================================================== */
/*  Decodificador UTF-8 -> codepoint Unicode                          */
/* ================================================================== */
static int utf8_dec(const char *s, size_t n, size_t *pi) {
    const unsigned char *u = (const unsigned char *)s;
    size_t i = *pi;
    unsigned char c = u[i];
    int cp, extra;
    if (c < 0x80) { *pi = i + 1; return c; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { *pi = i + 1; return '?'; }
    if (i + 1 + (size_t)extra > n) { *pi = n; return '?'; }
    i++;
    for (int k = 0; k < extra; k++) {
        unsigned char cc = u[i + k];
        if ((cc & 0xC0) != 0x80) { *pi = i + k; return '?'; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *pi = i + (size_t)extra;
    return cp;
}

/* ================================================================== */
/*  Mapeo de codepoints Unicode a WinAnsi                             */
/* ================================================================== */
static int cp2win(int cp) {
    if (cp < 0x80) return cp;
    if (cp >= 0xA0 && cp <= 0xFF) return cp;
    switch (cp) {
        case 0x20AC: return 0x80; case 0x201A: return 0x82; case 0x0192: return 0x83;
        case 0x201E: return 0x84; case 0x2026: return 0x85; case 0x2020: return 0x86;
        case 0x2021: return 0x87; case 0x02C6: return 0x88; case 0x2030: return 0x89;
        case 0x0160: return 0x8A; case 0x2039: return 0x8B; case 0x0152: return 0x8C;
        case 0x017D: return 0x8E; case 0x2018: return 0x91; case 0x2019: return 0x92;
        case 0x201C: return 0x93; case 0x201D: return 0x94; case 0x2022: return 0x95;
        case 0x2013: return 0x96; case 0x2014: return 0x97; case 0x02DC: return 0x98;
        case 0x2122: return 0x99; case 0x0161: return 0x9A; case 0x203A: return 0x9B;
        case 0x0153: return 0x9C; case 0x017E: return 0x9E; case 0x0178: return 0x9F;
    }
    return -1;
}

/* ================================================================== */
/*  Detección de texto UTF-8 válido                                   */
/* ================================================================== */
static int seems_utf8(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        int extra;
        if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return 0;
        if (i + (size_t)extra >= n) return 0;
        for (int k = 1; k <= extra; k++)
            if ((s[i + k] & 0xC0) != 0x80) return 0;
        i += 1 + (size_t)extra;
    }
    return 1;
}

/* ================================================================== */
/*  Detección de emojis y sus modificadores                           */
/* ================================================================== */
static int is_emoji_cp(int cp) {
    if (cp >= 0x1F600 && cp <= 0x1F64F) return 1;
    if (cp >= 0x1F300 && cp <= 0x1F5FF) return 1;
    if (cp >= 0x1F680 && cp <= 0x1F6FF) return 1;
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return 1;
    if (cp >= 0x1F900 && cp <= 0x1F9FF) return 1;
    if (cp >= 0x1FA70 && cp <= 0x1FAFF) return 1;
    if (cp >= 0x2600 && cp <= 0x26FF) return 1;
    if (cp >= 0x2700 && cp <= 0x27BF) return 1;
    if (cp >= 0x2300 && cp <= 0x23FF) return 1;
    if (cp == 0x200D) return 1;
    if (cp == 0xFE0F) return 1;
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
    if (cp == 0x20E3) return 1;
    return 0;
}

static int is_emoji_modifier(int cp) {
    if (cp == 0x200D) return 1;
    if (cp == 0xFE0F) return 1;
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return 1;
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return 1;
    if (cp == 0x20E3) return 1;
    return 0;
}

/* ================================================================== */
/*  Codificador codepoint -> UTF-8                                    */
/* ================================================================== */
static int cp_to_utf8(int cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ================================================================== */
/*  Conversión de línea preservando secuencias de emoji completas     */
/* ================================================================== */
static char *to_winansi(const char *s) {
    size_t n = strlen(s);
    char *out = (char *)malloc(n * 4 + 1);
    size_t o = 0;
    if (g_utf8) {
        size_t i = 0;
        while (i < n) {
            int cp = utf8_dec(s, n, &i);
            if (cp == 0x200B || cp == 0x200C || cp == 0x00AD) continue;
            if (cp == '\t') { out[o++] = ' '; continue; }
            if (is_emoji_cp(cp)) {
                char tmp[4];
                int tl = cp_to_utf8(cp, tmp);
                memcpy(out + o, tmp, (size_t)tl);
                o += (size_t)tl;
                while (i < n) {
                    size_t peek_i = i;
                    int next_cp = utf8_dec(s, n, &peek_i);
                    if (is_emoji_modifier(next_cp)) {
                        char tmp2[4];
                        int tl2 = cp_to_utf8(next_cp, tmp2);
                        memcpy(out + o, tmp2, (size_t)tl2);
                        o += (size_t)tl2;
                        i = peek_i;
                    } else {
                        break;
                    }
                }
                continue;
            }
            int w = cp2win(cp);
            if (w >= 0) out[o++] = (char)w;
            else if (cp < 0x2000) out[o++] = '?';
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '\t') { out[o++] = ' '; continue; }
            if (c == 0x81 || c == 0x8D || c == 0x8F || c == 0x90 || c == 0x9D || c == 0xAD) continue;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return out;
}

/* ================================================================== */
/*  Lectura completa de archivo                                       */
/* ================================================================== */
static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *b = (char *)malloc((size_t)n + 1);
    size_t r = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[r] = 0;
    if (len) *len = r;
    return b;
}

static int starts_with_html_img(const char *s) {
    if (s[0] != '<') return 0;
    unsigned char c1 = (unsigned char)s[1];
    unsigned char c2 = (unsigned char)s[2];
    unsigned char c3 = (unsigned char)s[3];
    if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
    if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
    if (c3 >= 'A' && c3 <= 'Z') c3 += 32;
    if (c1 != 'i' || c2 != 'm' || c3 != 'g') return 0;
    return s[4] == ' ' || s[4] == '\t' || s[4] == '>';
}

/* ================================================================== */
/*  Función principal                                                 */
/* ================================================================== */
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "micro-md2pdf (Markdown -> PDF)\nuse: %s input.md output.pdf\n", argv[0]);
        return 1;
    }

    size_t flen;
    char *src = read_file(argv[1], &flen);
    if (!src) { fprintf(stderr, "error: I cannot read %s\n", argv[1]); return 1; }

    char *s0 = src;
    if (flen >= 3 && (unsigned char)src[0] == 0xEF &&
        (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF)
        s0 += 3;

    g_utf8 = seems_utf8((const unsigned char *)s0, strlen(s0));

    /* Construir el atlas dinámico de emojis ANTES de parsear */
    emoji_atlas_build(s0);

    Doc doc = {0};
    new_page(&doc);

    Buf para = {0};
    int have_para = 0;
    Buf code = {0};
    int in_code = 0;
    Row *rows = NULL;
    int rn = 0, rc = 0;
    char *linebuf = (char *)malloc(flen + 2);
    char *p = s0;
    int prev_hb = 0;

    while (*p) {
        char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        memcpy(linebuf, p, ll);
        linebuf[ll] = 0;
        p += ll;
        if (*p == '\n') p++;

        size_t L = strlen(linebuf);
        if (L && linebuf[L - 1] == '\r') linebuf[L - 1] = 0;

        char *ln = to_winansi(linebuf);
        size_t ln_len = strlen(ln);
        int hard_break = (ln_len >= 2 &&
                          ln[ln_len - 1] == ' ' &&
                          ln[ln_len - 2] == ' ');

        for (;;) {
            if (in_code) {
                if (!strncmp(ln, "```", 3)) {
                    code_block(&doc, &code);
                    code.n = 0;
                    if (code.d) code.d[0] = 0;
                    in_code = 0;
                } else {
                    bputs(&code, ln);
                    bputc(&code, '\n');
                }
                break;
            }

            if (!strncmp(ln, "```", 3)) {
                flush_para(&doc, &para, &have_para);
                in_code = 1;
                break;
            }

            if (is_table_line(ln)) {
                flush_para(&doc, &para, &have_para);
                Row r = mk_row(ln);
                if (is_sep_row(&r)) free_row(&r);
                else {
                    if (rn == rc) {
                        rc = rc ? rc * 2 : 16;
                        rows = (Row *)realloc(rows, (size_t)rc * sizeof(Row));
                    }
                    rows[rn++] = r;
                }
                break;
            }

            if (rn) {
                render_table(&doc, rows, rn);
                free_rows(rows, rn);
                rows = NULL;
                rn = rc = 0;
            }

            if (blank(ln)) { flush_para(&doc, &para, &have_para); break; }

            char *t = ln;
            while (*t == ' ' || *t == '\t') t++;

            if (*t == '#') {
                int lvl = 0;
                while (t[lvl] == '#') lvl++;
                if (t[lvl] == ' ' || t[lvl] == '\t') {
                    flush_para(&doc, &para, &have_para);
                    heading(&doc, lvl, trim(t + lvl + 1));
                    break;
                }
            }

            if (is_hr(t)) { flush_para(&doc, &para, &have_para); hr(&doc); break; }

            if (*t == '>') {
                flush_para(&doc, &para, &have_para);
                char *q = t;
                while (*q == '>') q++;
                while (*q == ' ' || *q == '\t') q++;
                quote_line(&doc, q);
                break;
            }

            /* ---- Imagen de bloque: la línea contiene SOLO una imagen ---- */
{
    char iurl[1024];
    int wpct = 0, wpx = 0;
    int is_block = 0;

    if (t[0] == '!' && t[1] == '[') {
        const char *cl = strchr(t + 2, ']');
        if (cl && cl[1] == '(') {
            int depth = 1;
            const char *q = cl + 2;
            while (*q && depth > 0) {
                if (*q == '(') depth++;
                else if (*q == ')') depth--;
                if (depth > 0) q++;
            }
            if (depth == 0) {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                if (*q == 0 && image_url(t, iurl, sizeof iurl, &wpct, &wpx))
                    is_block = 1;
            }
        }
    } else if (starts_with_html_img(t)) {
        const char *gt = strchr(t, '>');
        if (gt) {
            const char *after = gt + 1;
            while (*after == ' ' || *after == '\t') after++;
            if (*after == 0 && image_url(t, iurl, sizeof iurl, &wpct, &wpx))
                is_block = 1;
        }
    }

    if (is_block) {
        flush_para(&doc, &para, &have_para);
        render_image(&doc, iurl, wpct, wpx);   /* ← tamaño completo */
        break;
    }
}

            {
                char iurl[1024];
                int wpct = 0, wpx = 0;
                int found_image = 0;
                const char *scan = t;
                double curx = ML;
                double cury = doc.y;
                double maxh = 0;

                while (*scan) {
                    const char *bang = strstr(scan, "![");
                    if (!bang) break;
                    if (!image_url(bang, iurl, sizeof iurl, &wpct, &wpx)) {
                        scan = bang + 2;
                        continue;
                    }
                    if (!found_image) flush_para(&doc, &para, &have_para);
                    render_image_inline(&doc, iurl, &curx, &cury, &maxh);
                    found_image = 1;

                    const char *cl = strchr(bang + 2, ']');
                    if (!cl || cl[1] != '(') { scan = bang + 2; continue; }
                    int depth = 1;
                    const char *q = cl + 2;
                    while (*q && depth > 0) {
                        if (*q == '(') depth++;
                        else if (*q == ')') depth--;
                        if (depth > 0) q++;
                    }
                    if (depth != 0) { scan = bang + 2; continue; }
                    scan = q + 1;
                    if (*scan == ']' && scan[1] == '(') {
                        scan += 2;
                        depth = 1;
                        while (*scan && depth > 0) {
                            if (*scan == '(') depth++;
                            else if (*scan == ')') depth--;
                            if (depth > 0) scan++;
                        }
                        if (depth == 0) scan++;
                    }
                }
                if (found_image) {
                    doc.y = cury - maxh - 10.0;
                    break;
                }
            }

            if ((*t == '-' || *t == '*' || *t == '+') && t[1] == ' ') {
                flush_para(&doc, &para, &have_para);
                list_item(&doc, "\225", trim(t + 2));
                break;
            }

            if (t[0] >= '0' && t[0] <= '9') {
                char *q = t;
                while (*q >= '0' && *q <= '9') q++;
                if (*q == '.' && (q[1] == ' ' || q[1] == '\t')) {
                    flush_para(&doc, &para, &have_para);
                    char mk[24];
                    size_t dl = (size_t)(q - t);
                    if (dl > 20) dl = 20;
                    memcpy(mk, t, dl);
                    mk[dl] = '.';
                    mk[dl + 1] = 0;
                    list_item(&doc, mk, trim(q + 1));
                    break;
                }
            }

            if (!have_para) prev_hb = 0;
            if (have_para) {
                if (prev_hb) bputs(&para, "<br>");
                else         bputc(&para, ' ');
            }
            bputs(&para, trim(ln));
            have_para = 1;
            prev_hb    = hard_break;
            break;
        }

        free(ln);
    }

    if (in_code) code_block(&doc, &code);
    if (rn) { render_table(&doc, rows, rn); free_rows(rows, rn); }
    if (have_para) para_text(&doc, para.d);

    int np = write_pdf(&doc, argv[2]);
    if (np < 0) { fprintf(stderr, "error: I cannot write %s\n", argv[2]); return 1; }
    fprintf(stderr, "ok: %d page(s) -> %s\n", np, argv[2]);

    free_images();
    free(para.d);
    free(code.d);
    free(linebuf);
    free(src);
    return 0;
}
