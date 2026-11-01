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

#include <stdint.h>
#include "parser.h"
/* Altura de los badges/shields en puntos (ajustable con --height) */
#ifndef BADGE_H_PT
#define BADGE_H_PT 12.0
#endif

/* Prototipos del motor gráfico de emojis (implementados en image.c) */
void render_emoji_nativo(Doc *d, int idx, double x_pdf, double y_pdf, double size_pts);
int  buscar_secuencia_emoji(const uint32_t *src_secuencia, uint8_t src_len);
int img_get(const char *src, int wpct, int wpx); 


#define EMOJI_SEQ_MAX 16

/* Decodificador UTF-8 -> codepoint Unicode */
static int read_utf8_codepoint(const char *s, size_t n, size_t *i, uint32_t *cp) {
    if (*i >= n) return 0;
    unsigned char c = (unsigned char)s[*i];
    if (c < 0x80) {
        *cp = c; *i += 1; return 1;
    } else if ((c & 0xE0) == 0xC0 && *i + 1 < n) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[*i + 1] & 0x3F);
        *i += 2; return 2;
    } else if ((c & 0xF0) == 0xE0 && *i + 2 < n) {
        *cp = ((uint32_t)(c & 0x0F) << 12) |
              ((uint32_t)(s[*i + 1] & 0x3F) << 6) |
              ((uint32_t)(s[*i + 2] & 0x3F));
        *i += 3; return 3;
    } else if ((c & 0xF8) == 0xF0 && *i + 3 < n) {
        *cp = ((uint32_t)(c & 0x07) << 18) |
              ((uint32_t)(s[*i + 1] & 0x3F) << 12) |
              ((uint32_t)(s[*i + 2] & 0x3F) << 6) |
              ((uint32_t)(s[*i + 3] & 0x3F));
        *i += 4; return 4;
    }
    *cp = 0xFFFD; *i += 1; return 1;
}

/* Descodifica los bytes UTF-8 de un Seg de emoji y lo pinta */
static void render_emoji_seg(Doc *d, Seg *g, double curx, double b, double fs, double segwidth) {
    uint32_t seq[EMOJI_SEQ_MAX];
    uint8_t  seq_len = 0;
    size_t   temp_i = 0;
    size_t   str_len = strlen(g->txt);
    while (temp_i < str_len && seq_len < EMOJI_SEQ_MAX) {
        uint32_t cp = 0;
        int len = read_utf8_codepoint(g->txt, str_len, &temp_i, &cp);
        if (!len) break;
        seq[seq_len++] = cp;
    }
    int idx = buscar_secuencia_emoji(seq, seq_len);
    if (idx >= 0) render_emoji_nativo(d, idx, curx, b - fs * 0.15, segwidth);
}

static void run_add_url(Runs *R, int style, const char *s, size_t len, const char *url) {
    if (!len) return;
    if (R->n == R->cap) {
        R->cap = R->cap ? R->cap * 2 : 8;
        R->v = (Run *)realloc(R->v, (size_t)R->cap * sizeof(Run));
    }
    Run *r = &R->v[R->n++];
    r->style = style;
    binit(&r->txt);
    bput(&r->txt, s, len);
    r->url[0] = 0;
    if (url) {
        strncpy(r->url, url, sizeof(r->url) - 1);
        r->url[sizeof(r->url) - 1] = 0;
    }
}

static void run_add(Runs *R, int style, const char *s, size_t len) {
    run_add_url(R, style, s, len, NULL);
}

void free_runs(Runs *R) {
    for (int i = 0; i < R->n; i++) free(R->v[i].txt.d);
    free(R->v); R->v = NULL; R->n = R->cap = 0;
}

/* Detecta <br>, <br/>, <br /> (case-insensitive). Devuelve longitud o 0. */
static int match_br(const char *src, size_t n, size_t i) {
    if (i + 3 >= n) return 0;
    if (src[i] != '<') return 0;
    char c1 = src[i + 1], c2 = src[i + 2];
    if (!((c1 == 'b' || c1 == 'B') && (c2 == 'r' || c2 == 'R'))) return 0;
    if (src[i + 3] == '>') return 4;
    if (i + 4 < n && src[i + 3] == '/' && src[i + 4] == '>') return 5;
    if (i + 5 < n && src[i + 3] == ' ' && src[i + 4] == '/' && src[i + 5] == '>') return 6;
    return 0;
}

void parse_inline(const char *src, Runs *R) {
    size_t n = strlen(src);
    int bold = 0, it = 0, code = 0;
    size_t i = 0;
    #define ST() (code ? 4 : ((bold ? 1 : 0) | (it ? 2 : 0)))
    Buf cur = {0};

    while (i < n) {
        char c = src[i];

        /* 0. Saltos duros HTML (<br>, <br/>, <br />) */
        if (!code && c == '<') {
            int bl = match_br(src, n, i);
            if (bl) {
                if (cur.n) { run_add(R, ST(), cur.d, cur.n); cur.n = 0; if (cur.d) cur.d[0] = 0; }
                run_add(R, 128, "\n", 1);
                i += bl;
                continue;
            }
        }

        /* 1. Escape character (\) */
        if (c == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!|~>", src[i + 1])) {
            bputc(&cur, src[i + 1]);
            i += 2;
            continue;
        }

     /* 1.5 Imágenes inline ![alt](url): un único run con texto=URL (style 32) */
     if (!code && c == '!' && i + 1 < n && src[i + 1] == '[') {
         const char *cl = strchr(src + i + 2, ']');
         if (cl && cl[1] == '(') {
             const char *cp = cl + 2; int depth = 1;
             while (*cp && depth > 0) {
                 if (*cp == '(') depth++; else if (*cp == ')') depth--;
                 if (depth > 0) cp++;
             }
             if (depth == 0) {
                 if (cur.n) { run_add(R, ST(), cur.d, cur.n); cur.n = 0; if (cur.d) cur.d[0] = 0; }
                 char urlbuf[2048];
                 size_t ulen = (size_t)(cp - cl - 2);
                 if (ulen >= sizeof(urlbuf)) ulen = sizeof(urlbuf) - 1;
                 memcpy(urlbuf, cl + 2, ulen); urlbuf[ulen] = 0;
                 char *sp = strchr(urlbuf, ' '); if (sp) *sp = 0;
                 if (urlbuf[0]) {
                     run_add_url(R, ST() | 8 | 32, urlbuf, strlen(urlbuf), urlbuf);
                     i = (size_t)(cp - src) + 1;
                     continue;
                 }
             }
         }
     }
        /* 2. Inline code (`) */
        if (c == '`') {
            if (cur.n) { run_add(R, ST(), cur.d, cur.n); cur.n = 0; if (cur.d) cur.d[0] = 0; }
            code = !code;
            i++;
            continue;
        }

        /* 3. Bold / Italic (* o _) */
        if (!code && (c == '*' || c == '_')) {
            int dbl = (i + 1 < n && src[i + 1] == c);
            if (cur.n) { run_add(R, ST(), cur.d, cur.n); cur.n = 0; if (cur.d) cur.d[0] = 0; }
            if (dbl) bold = !bold; else it = !it;
            i += dbl ? 2 : 1;
            continue;
        }

        /* 4. Autolinks ( <http...> ) */
        if (!code && c == '<') {
            const char *close = strchr(src + i + 1, '>');
            if (close) {
                size_t ulen = (size_t)(close - src - i - 1);
                const char *url_start = src + i + 1;
                if (ulen > 7 && (strncmp(url_start, "http://", 7) == 0 ||
                                 strncmp(url_start, "https://", 8) == 0)) {
                    if (cur.n) { run_add(R, ST(), cur.d, cur.n); cur.n = 0; if (cur.d) cur.d[0] = 0; }
                    char urlbuf[2048];
                    if (ulen >= sizeof(urlbuf)) ulen = sizeof(urlbuf) - 1;
                    memcpy(urlbuf, url_start, ulen);
                    urlbuf[ulen] = 0;
                    run_add_url(R, ST() | 8, url_start, ulen, urlbuf);
                    i = (size_t)(close - src) + 1;
                    continue;
                }
            }
        }

        /* 5. Links [text](url) */
        if (!code && c == '[') {
            const char *cl = strchr(src + i, ']');
            if (cl && cl[1] == '(') {
                const char *cp = cl + 2;
                int depth = 1;
                while (*cp && depth > 0) {
                    if (*cp == '(') depth++;
                    else if (*cp == ')') depth--;
                    if (depth > 0) cp++;
                }
                if (depth == 0) {
                 /* Badge envuelto en enlace: [![alt](img)](link) */
                 if (src[i + 1] == '!' && src[i + 2] == '[' && *cp == ')' && cp[1] == '(') {
                     const char *op = cp + 2; int d2 = 1; const char *oq = op;
                     while (*oq && d2 > 0) {
                         if (*oq == '(') d2++; else if (*oq == ')') d2--;
                         if (d2 > 0) oq++;
                     }
                     if (d2 == 0) {
                         char imgurl[2048];
                         size_t il = (size_t)(cp - cl - 2);
                         if (il >= sizeof(imgurl)) il = sizeof(imgurl) - 1;
                         memcpy(imgurl, cl + 2, il); imgurl[il] = 0;
                         char *s1 = strchr(imgurl, ' '); if (s1) *s1 = 0;
                         if (imgurl[0]) {
                             char linkurl[2048];
                             size_t ll = (size_t)(oq - op);
                             if (ll >= sizeof(linkurl)) ll = sizeof(linkurl) - 1;
                             memcpy(linkurl, op, ll); linkurl[ll] = 0;
                             char *s2 = strchr(linkurl, ' '); if (s2) *s2 = 0;
                             if (cur.n) { run_add(R, ST(), cur.d, cur.n); cur.n = 0; if (cur.d) cur.d[0] = 0; }
                             run_add_url(R, ST() | 8 | 32, imgurl, strlen(imgurl),
                                         linkurl[0] ? linkurl : imgurl);
                             i = (size_t)(oq - src) + 1;
                             continue;
                         }
                     }
                 }
                    if (cur.n) { run_add(R, ST(), cur.d, cur.n); cur.n = 0; if (cur.d) cur.d[0] = 0; }
                    char urlbuf[2048];
                    size_t ulen = (size_t)(cp - cl - 2);
                    if (ulen >= sizeof(urlbuf)) ulen = sizeof(urlbuf) - 1;
                    memcpy(urlbuf, cl + 2, ulen);
                    urlbuf[ulen] = 0;
                    char *sp = strchr(urlbuf, ' ');
                    if (sp && (strchr(sp, '"') || strchr(sp, '\''))) sp = 0;
                    run_add_url(R, ST() | 8, src + i + 1, (size_t)(cl - src - i - 1), urlbuf);
                    i = (size_t)(cp - src) + 1;
                    continue;
                }
            }
        }

        /* 6. Bare URLs (http...) */
        if (!code && c == 'h' && i + 8 <= n) {
            int is_http = 0;
            if (strncmp(src + i, "http://", 7) == 0) is_http = 7;
            else if (strncmp(src + i, "https://", 8) == 0) is_http = 8;
            if (is_http) {
                size_t start = i;
                size_t end = i + is_http;
                while (end < n && src[end] != ' ' && src[end] != '\t' &&
               src[end] != '\n' && src[end] != '\r' &&
               src[end] != '<' && src[end] != '>' && 
               (unsigned char)src[end] < 0x80) { 
            end++;
            }

                size_t url_len = end - start;
                const char *url_str = src + start;
                int open_parens = 0;
                for (size_t k = 0; k < url_len; k++) {
                    if (url_str[k] == '(') open_parens++;
                    else if (url_str[k] == ')') open_parens--;
                }
                while (url_len > 0 && open_parens < 0 && url_str[url_len - 1] == ')') {
                    url_len--;
                    open_parens++;
                }
                while (url_len > 0) {
                    char last = url_str[url_len - 1];
                    if (last == '.' || last == ',' || last == ';' || last == ':' ||
                        last == '!' || last == '?') {
                        url_len--;
                    } else if (url_len >= 2 &&
                               (unsigned char)url_str[url_len - 2] == 0xC2 &&
                               (unsigned char)url_str[url_len - 1] == 0xB7) {
                        url_len -= 2;
                    } else {
                        break;
                    }
                }
                if (url_len > (size_t)is_http) {
                    if (cur.n) { run_add(R, ST(), cur.d, cur.n); cur.n = 0; if (cur.d) cur.d[0] = 0; }
                    char urlbuf[2048];
                    size_t blen = url_len;
                    if (blen >= sizeof(urlbuf)) blen = sizeof(urlbuf) - 1;
                    memcpy(urlbuf, url_str, blen);
                    urlbuf[blen] = 0;
                    run_add_url(R, ST() | 8, url_str, url_len, urlbuf);
                    i = start + url_len; 
                   continue;
                }
            }
        }

        /* BLOQUE GREEDY DE DETECCIÓN DE EMOJIS UTF-8 */
        if (!code && (unsigned char)c >= 0x80) {
            uint32_t seq[EMOJI_SEQ_MAX];
            uint8_t  seq_len = 0;
            size_t   temp_i = i;
            size_t   bytes_consumed = 0;
            uint32_t cp = 0;
            int len = read_utf8_codepoint(src, n, &temp_i, &cp);
            if (len > 0) {
                seq[seq_len++] = cp;
                bytes_consumed += (size_t)len;
                while (seq_len < EMOJI_SEQ_MAX && temp_i < n) {
                    size_t   next_i = temp_i;
                    uint32_t next_cp = 0;
                    int next_len = read_utf8_codepoint(src, n, &next_i, &next_cp);
                    if (next_len == 0) break;

                    int is_zwj      = (next_cp == 0x200D);
                    int is_regional = (next_cp >= 0x1F1E6 && next_cp <= 0x1F1FF);
                    int is_vs16     = (next_cp == 0xFE0F);
                    int is_keycap   = (next_cp == 0x20E3);
                    int is_skin     = (next_cp >= 0x1F3FB && next_cp <= 0x1F3FF);

                    if (is_zwj || is_regional || is_vs16 || is_keycap || is_skin) {
                        seq[seq_len++] = next_cp;
                        temp_i = next_i;
                        bytes_consumed += (size_t)next_len;
                        if (is_zwj && seq_len < EMOJI_SEQ_MAX && temp_i < n) {
                            next_i = temp_i;
                            next_len = read_utf8_codepoint(src, n, &next_i, &next_cp);
                            if (next_len > 0) {
                                seq[seq_len++] = next_cp;
                                temp_i = next_i;
                                bytes_consumed += (size_t)next_len;
                            }
                        }
                    } else {
                        break;
                    }
                }

                int idx = buscar_secuencia_emoji(seq, seq_len);
                if (idx == -1 && seq_len > 1) {
                    size_t   first_i = i;
                    uint32_t first_cp = 0;
                    int first_len = read_utf8_codepoint(src, n, &first_i, &first_cp);
                    uint32_t first_seq[1] = { first_cp };
                    idx = buscar_secuencia_emoji(first_seq, 1);
                    if (idx != -1) {
                        bytes_consumed = (size_t)first_len;
                        seq_len = 1;
                    }
                }
                if (idx != -1) {
                    if (cur.n) { run_add(R, ST(), cur.d, cur.n); cur.n = 0; if (cur.d) cur.d[0] = 0; }
                    run_add(R, ST() | 64, src + i, bytes_consumed);
                    i += bytes_consumed;
                    continue;
                }
            }

            /* FIX: skip full codepoint if valid UTF-8 multibyte and no emoji match */
            if ((unsigned char)c >= 0xC0) {
                size_t   skip_i = i;
                uint32_t skip_cp = 0;
                int skip_len = read_utf8_codepoint(src, n, &skip_i, &skip_cp);
                int valid = (skip_len > 1);
                for (int k = 1; k < skip_len && valid; k++) {
                    if (((unsigned char)src[i + (size_t)k] & 0xC0) != 0x80)
                        valid = 0;
                }
                if (valid) {
    size_t num_bytes = skip_i - i;
    for(size_t k = 0; k < num_bytes; k++) {
        bputc(&cur, src[i + k]);
    }
    i = skip_i;
    continue;
}

            }
        }

        bputc(&cur, c);
        i++;
    }
    if (cur.n) run_add(R, ST(), cur.d, cur.n);
    free(cur.d);
    #undef ST
}

/* Devuelve 1 si la cadena es una URL de imagen reconocible (badge/include) */
static int is_img_url(const char *s) {
    return s && (strstr(s, "img.shields.io") || strstr(s, ".png") || strstr(s, ".svg"));
}

double segw(const char *s, size_t len, int style, double fs) {
    if (style & 64) return fs * 1.15;
    
    /* MAGIA: solo si el segmento es una imagen inline marcada (style & 32) */
if ((style & 32) && is_img_url(s)) {
        int id = img_get(s, 0, 0);
        if (id >= 0 && imgs[id].rw > 0 && imgs[id].rh > 0) {
            double hp = ((double)imgs[id].rh / 96.0) * 72.0;
            double wp = ((double)imgs[id].rw / 96.0) * 72.0;
            double sc = BADGE_H_PT / hp; // Escalado a la altura del badge
            return wp * sc + 4.0;  // Ancho real más espacio de separación
        }
    }

    int mono = style & 4; double w = 0;
    for (size_t k = 0; k < len; k++) w += glyph_w((unsigned char)s[k], mono, style);
    return w / 1000.0 * fs;
}


double longest_word(const char *s, double fs) {
    double best = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *e = p; while (*e && *e != ' ') e++;
        double w = segw(p, (size_t)(e - p), 0, fs);
        if (w > best) best = w;
        p = e;
    }
    return best;
}

static void seg_add(Line2 *l, int style, const char *s, size_t len, const char *url) {
    if (!len) return;
    /* FIX: no fusionar segmentos de emoji */
    if (!(style & 64) && l->n && l->v[l->n - 1].style == style) {
        Seg *g = &l->v[l->n - 1];
        int same_url = (!url && !g->url[0]) || (url && g->url[0] && strcmp(g->url, url) == 0);
        if (same_url) {
            size_t o = strlen(g->txt);
            g->txt = (char *)realloc(g->txt, o + len + 1);
            memcpy(g->txt + o, s, len); g->txt[o + len] = 0;
            return;
        }
    }
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = (Seg *)realloc(l->v, (size_t)l->cap * sizeof(Seg));
    }
    Seg *g = &l->v[l->n++];
    g->style = style;
    g->txt = (char *)malloc(len + 1);
    memcpy(g->txt, s, len); g->txt[len] = 0;
    g->url[0] = 0;
    if (url && url[0]) {
        strncpy(g->url, url, sizeof(g->url) - 1);
        g->url[sizeof(g->url) - 1] = 0;
    }
}

int wrap_runs(Runs *R, double maxw, double fs, Line2 **out) {
    Line2 *L = NULL; int nl = 0, lc = 0;
    Line2 cur = {0}, z = {0};
    double curw = 0; int pend = 0;
    #define FLUSH() do { \
        if (nl == lc) { lc = lc ? lc * 2 : 16; L = (Line2 *)realloc(L, (size_t)lc * sizeof(Line2)); } \
        L[nl++] = cur; cur = z; curw = 0; pend = 0; \
    } while(0)
    for (int ri = 0; ri < R->n; ri++) {
        if (R->v[ri].style & 128) {
            if (cur.n) FLUSH();
            continue;
        }
        int style = R->v[ri].style, mono = style & 4;
        const char *run_url = R->v[ri].url[0] ? R->v[ri].url : NULL;
        double spw = glyph_w(' ', mono, style) / 1000.0 * fs;
        const char *p = R->v[ri].txt.d ? R->v[ri].txt.d : " ";
        while (*p) {
            int sp = 0;
            while (*p == ' ') { p++; sp = 1; }
            if (!*p) { if (sp && (cur.n || curw > 0)) pend = 1; break; }
            if (sp && (cur.n || curw > 0)) pend = 1;
            const char *e = p; while (*e && *e != ' ') e++;
            size_t wl = (size_t)(e - p);
            double ww = segw(p, wl, style, fs);
            if (cur.n && curw + (pend ? spw : 0) + ww > maxw) FLUSH();
            if (pend  && cur.n) { seg_add( &cur, (style & 64) ? 0 : style,  " ", 1, NULL); curw += spw; }
            pend = 0;
            if (ww > maxw) {
                const char *q = p; size_t left = wl;
                while (left) {
                    double rem = maxw - curw, acc = 0; size_t k = 0;
                    while (k < left) {
                    double cw = glyph_w((unsigned char)q[k], mono, style) / 1000.0 * fs;
                        if (acc + cw > rem && k > 0) break;
                        acc += cw; k++;
                    }
                    if (!k) k = 1;
                    seg_add(&cur, style, q, k, run_url); curw += acc;
                    q += k; left -= k;
                    if (left) FLUSH();
                }
            } else {
                seg_add(&cur, style, p, wl, run_url); curw += ww;
            }
            p = e;
        }
    }
    if (cur.n) FLUSH();
    #undef FLUSH
    *out = L;
    return nl;
}

void free_lines(Line2 *L, int nl) {
    for (int i = 0; i < nl; i++) {
        for (int j = 0; j < L[i].n; j++) free(L[i].v[j].txt);
        free(L[i].v);
    }
    free(L);
}

static const char *font_of(int st) {
    if (st & 4) return "F4";
    if (st & 1) return "F2";
    if (st & 2) return "F3";
    return "F1";
}

static void render_lines(Doc *d, Line2 *L, int nl, double x, double fs, double lh) {
    for (int i = 0; i < nl; i++) {
        room(d, lh);
        Buf *pg = CURPG(d);
        double b = d->y - fs * 0.84;
        double curx = x;

        /* Pass 1: subrayados, links, emojis E IMÁGENES INLINE / BADGES */
        for (int j = 0; j < L[i].n; j++) {
            Seg *g = &L[i].v[j]; // <--- La variable 'g' se declara AQUÍ
            
            // --- CÓDIGO DE CORRECCIÓN PARA EL PASS 1 ---
            const char *img_src = NULL;
         if (g->style & 32) img_src = g->txt;
         else if (g->url[0] && is_img_url(g->url)) img_src = g->url;
         if (img_src) {
             int img_id = img_get(img_src, 0, 0);
             if (img_id >= 0 && imgs[img_id].rw > 0 && imgs[img_id].rh > 0) {
                 double hp = ((double)imgs[img_id].rh / 96.0) * 72.0;
                 double wp = ((double)imgs[img_id].rw / 96.0) * 72.0;
                 double sc = BADGE_H_PT / hp; /* misma altura que reserva segw() */
                 wp *= sc; hp = BADGE_H_PT;
                 use_img(d, img_id);
                 bprintf(pg, "q %.2f 0 0 %.2f %.2f %.2f cm /Im%d Do Q\n", wp, hp, curx, b - 1.5, img_id);
                 if (g->url[0]) add_link(d, curx, b - 1.5, curx + wp, b + hp - 1.5, g->url);
                 curx += wp + 4.0;
                 continue;
             }
         }

            // Código original del Pass 1
            double segwidth = segw(g->txt, strlen(g->txt), g->style, fs);
            if (g->style & 64) {
                render_emoji_seg(d, g, curx, b, fs, segwidth);
                curx += segwidth;
                continue;
            }
            if (g->url[0]) {
                bprintf(pg, "0 0 1 RG 0.5 w %.2f %.2f m %.2f %.2f l S 0 0 0 RG\n",
                        curx, b - 1.5, curx + segwidth, b - 1.5);
                add_link(d, curx, b - 2.0, curx + segwidth, b + fs * 0.8, g->url);
            }
            curx += segwidth;
        }

        /* Pass 2: renderizado exclusivo del texto imprimible */
        curx = x;
        for (int j = 0; j < L[i].n; j++) {
            Seg *g = &L[i].v[j]; // <--- La variable 'g' se declara AQUÍ también
            
            // --- CÓDIGO DE CORRECCIÓN PARA EL PASS 2 ---
            const char *img_src = NULL;
         if (g->style & 32) img_src = g->txt;
         else if (g->url[0] && is_img_url(g->url)) img_src = g->url;
         if (img_src) {
             int img_id = img_get(img_src, 0, 0);
             if (img_id >= 0 && imgs[img_id].rw > 0 && imgs[img_id].rh > 0) {
                 double hp = ((double)imgs[img_id].rh / 96.0) * 72.0;
                 double wp = ((double)imgs[img_id].rw / 96.0) * 72.0;
                 wp *= BADGE_H_PT / hp;
                 curx += wp + 4.0;
                 continue; /* badge: no pintar texto encima */
             }
         }
            
            // Omitir el signo '!' si queda suelto antes de un badge
            if (g->txt && strcmp(g->txt, "!") == 0) {
                double sw = segw(g->txt, strlen(g->txt), g->style, fs);
                curx += sw;
                continue;
            }

            // Código original del Pass 2
            double segwidth = segw(g->txt, strlen(g->txt), g->style, fs);
            if (g->style & 64) { curx += segwidth; continue; }
            bprintf(pg, "BT 1 0 0 1 %.2f %.2f Tm ", curx, b);
            if (g->url[0]) bputs(pg, "0 0 1 rg ");
            bprintf(pg, "/%s %.2f Tf (", font_of(g->style), fs);
            esc_pdf(pg, g->txt);
            bputs(pg, ") Tj 0 0 0 rg ET\n");
            curx += segwidth;
        }

        d->y -= lh;
    }
}

void para_text(Doc *d, const char *txt) {
    double fs = FS, lh = FS * 1.42;
    Runs R = {0};
    parse_inline(txt, &R);
    Line2 *L; int nl = wrap_runs(&R, CONTENT_W, fs, &L);
    render_lines(d, L, nl, ML, fs, lh);
    free_lines(L, nl); free_runs(&R);
    d->y -= 8;
}

void heading(Doc *d, int lvl, const char *txt) {
    static const double SZ[6] = {17, 14, 12, 11, 10.5, 10};
    if (lvl < 1) lvl = 1; if (lvl > 6) lvl = 6;
    double fs = SZ[lvl - 1];
    d->y -= (lvl == 1 ? 14 : lvl == 2 ? 11 : 8);
    Runs R = {0};
    parse_inline(txt, &R);
    for (int i = 0; i < R.n; i++) {
        int s = R.v[i].style;
        if (s & 128) continue;
        if (s & 64)      R.v[i].style = 64 | (s & 1);
        else if (s & 4)  R.v[i].style = 4;
        else             R.v[i].style = 1;
    }
    Line2 *L; int nl = wrap_runs(&R, CONTENT_W, fs, &L);
    render_lines(d, L, nl, ML, fs, fs * 1.25);
    free_lines(L, nl); free_runs(&R);
    if (lvl <= 2) {
        room(d, 8);
        Buf *pg = CURPG(d);
        bprintf(pg, "0.72 0.72 0.72 RG 0.9 w %.2f %.2f m %.2f %.2f l S 0 0 0 RG\n",
                ML, d->y - 2, PW - MR, d->y - 2);
        d->y -= 8;
    } else d->y -= 4;
}

void hr(Doc *d) {
    room(d, 16);
    Buf *pg = CURPG(d);
    double yy = d->y - 8;
    bprintf(pg, "0.7 0.7 0.7 RG 1 w %.2f %.2f m %.2f %.2f l S 0 0 0 RG\n",
            ML, yy, PW - MR, yy);
    d->y -= 16;
}

void quote_line(Doc *d, const char *txt) {
    double fs = FS, lh = FS * 1.42, ind = 12;
    Runs R = {0};
    parse_inline(txt, &R);

    for (int i = 0; i < R.n; i++) {
        int s = R.v[i].style;
        if ((s & 4) || (s & 64) || (s & 128)) continue;
        R.v[i].style = s | 2;
    }
    Line2 *L; int nl = wrap_runs(&R, CONTENT_W - ind, fs, &L);
    for (int i = 0; i < nl; i++) {
        room(d, lh);
        Buf *pg = CURPG(d);
        double top = d->y, b = top - fs * 0.84;
        bprintf(pg, "0.78 0.78 0.78 rg %.2f %.2f 2.6 %.2f f 0 0 0 rg\n",
                ML, top - lh, 2.6, lh);

        /* Pass 1 */
        double curx = ML + ind;
        for (int j = 0; j < L[i].n; j++) {
            Seg *g = &L[i].v[j];
            double sw = segw(g->txt, strlen(g->txt), g->style, fs);
            if (g->style & 64) {
                render_emoji_seg(d, g, curx, b, fs, sw);
                curx += sw;
                continue;
            }
            if (g->url[0]) {
                bprintf(pg, "0 0 1 RG 0.5 w %.2f %.2f m %.2f %.2f l S 0 0 0 RG\n",
                        curx, b - 1.5, curx + sw, b - 1.5);
                add_link(d, curx, b - 2.0, curx + sw, b + fs * 0.8, g->url);
            }
            curx += sw;
        }

        /* Pass 2 con Tm por segmento */
        curx = ML + ind;
        for (int j = 0; j < L[i].n; j++) {
            Seg *g = &L[i].v[j];
            double sw = segw(g->txt, strlen(g->txt), g->style, fs);
            if (g->style & 64) { curx += sw; continue; }
            bprintf(pg, "BT 1 0 0 1 %.2f %.2f Tm ", curx, b);
            if (g->url[0]) bputs(pg, "0 0 1 rg ");
            bprintf(pg, "/%s %.2f Tf (", font_of(g->style), fs);
            esc_pdf(pg, g->txt);
            bputs(pg, ") Tj 0 0 0 rg ET\n");
            curx += sw;
        }
        d->y -= lh;
    }
    free_lines(L, nl); free_runs(&R);
}

void list_item(Doc *d, const char *mk, const char *txt) {
    double fs = FS, lh = FS * 1.42, ind = 15;
    Runs R = {0};
    parse_inline(txt, &R);
    Line2 *L; int nl = wrap_runs(&R, CONTENT_W - ind, fs, &L);
    for (int i = 0; i < nl; i++) {
        room(d, lh);
        Buf *pg = CURPG(d);
        double b = d->y - fs * 0.84;
        double textx0 = ML + ind;

        /* Pass 1: emojis + underlines */
        double curx = textx0;
        for (int j = 0; j < L[i].n; j++) {
            Seg *g = &L[i].v[j];
            double sw = segw(g->txt, strlen(g->txt), g->style, fs);
            if (g->style & 64) {
                render_emoji_seg(d, g, curx, b, fs, sw);
                curx += sw;
                continue;
            }
            if (g->url[0]) {
                bprintf(pg, "0 0 1 RG 0.5 w %.2f %.2f m %.2f %.2f l S 0 0 0 RG\n",
                        curx, b - 1.5, curx + sw, b - 1.5);
                add_link(d, curx, b - 2.0, curx + sw, b + fs * 0.8, g->url);
            }
            curx += sw;
        }

        /* Marker (solo en la primera línea) */
        if (i == 0) {
            bprintf(pg, "BT 1 0 0 1 %.2f %.2f Tm /F1 %.2f Tf (", ML, b, fs);
            esc_pdf(pg, mk);
            bputs(pg, ") Tj ET\n");
        }

        /* Pass 2 con Tm por segmento */
        curx = textx0;
        for (int j = 0; j < L[i].n; j++) {
            Seg *g = &L[i].v[j];
            double sw = segw(g->txt, strlen(g->txt), g->style, fs);
            if (g->style & 64) { curx += sw; continue; }
            bprintf(pg, "BT 1 0 0 1 %.2f %.2f Tm ", curx, b);
            if (g->url[0]) bputs(pg, "0 0 1 rg ");
            bprintf(pg, "/%s %.2f Tf (", font_of(g->style), fs);
            esc_pdf(pg, g->txt);
            bputs(pg, ") Tj 0 0 0 rg ET\n");
            curx += sw;
        }
        d->y -= lh;
    }
    free_lines(L, nl); free_runs(&R);
    d->y -= 4;
}

void code_block(Doc *d, Buf *code) {
    double fs = 8.5, lh = fs * 1.35;
    char *s = code->d ? code->d : (char *)" ";
    char *p = s;
    d->y -= 2;
    while (*p) {
        char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        Runs R = {0};
        run_add(&R, 4, p, ll);
        Line2 *L; int nl = wrap_runs(&R, CONTENT_W - 8, fs, &L);
        if (nl == 0) { room(d, lh); d->y -= lh; }
        else render_lines(d, L, nl, ML + 4, fs, lh);
        free_lines(L, nl); free_runs(&R);
        p += ll; if (*p == '\n') p++;
    }
    d->y -= 8;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1; char *t = (char *)malloc(n);
    memcpy(t, s, n); return t;
}

Row mk_row(const char *line) {
    Row r = {0};
    Buf cur = {0};
    const char *s = line;
    while (*s == ' ') s++;
    if (*s == '|') s++;
    for (;;) {
        if (*s == '\\' && s[1] == '|') { bputc(&cur, '|'); s += 2; continue; }
        if (*s == '|' || !*s) {
            int end = !*s, has = 0;
            for (size_t k = 0; k < cur.n; k++)
                if (cur.d[k] != ' ' && cur.d[k] != '\t') { has = 1; break; }
            if (has || !end) {
                if (r.n == r.cap) {
                    r.cap = r.cap ? r.cap * 2 : 8;
                    r.c = (char **)realloc(r.c, (size_t)r.cap * sizeof(char *));
                }
                size_t a = 0, b = cur.n;
                while (a < b && (cur.d[a] == ' ' || cur.d[a] == '\t')) a++;
                while (b > a && (cur.d[b - 1] == ' ' || cur.d[b - 1] == '\t' || cur.d[b - 1] == '\r')) b--;
                char *t = (char *)malloc(b - a + 1);
                if (b > a) memcpy(t, cur.d + a, b - a);
                t[b - a] = 0;
                r.c[r.n++] = t;
            }
            cur.n = 0; if (cur.d) cur.d[0] = 0;
            if (end) break;
            s++; continue;
        }
        bputc(&cur, *s); s++;
    }
    free(cur.d);
    return r;
}

int is_sep_row(const Row *r) {
    if (r->n == 0) return 0;
    int ok = 0;
    for (int i = 0; i < r->n; i++) {
        int dash = 0, any = 0;
        for (const char *p = r->c[i]; *p; p++) {
            any = 1;
            if (*p == '-') dash = 1;
            else if (*p != ':' && *p != ' ') return 0;
        }
        if (any && dash) ok = 1;
    }
    return ok;
}

void free_row(Row *r) {
    for (int i = 0; i < r->n; i++) free(r->c[i]);
    free(r->c); r->c = NULL; r->n = r->cap = 0;
}

void free_rows(Row *rows, int n) {
    for (int i = 0; i < n; i++) free_row(&rows[i]);
    free(rows);
}

typedef struct { Line2 **L; int *N; int maxl; } Cells;

static Cells prep_cells(Row *r, int nc, const double *cw, double fs, int bold) {
    Cells C;
    C.L = (Line2 **)malloc((size_t)nc * sizeof(Line2 *));
    C.N = (int *)malloc((size_t)nc * sizeof(int));
    C.maxl = 1;
    for (int j = 0; j < nc; j++) {
        Runs R = {0};
        parse_inline(r->c[j], &R);
        if (bold)
            for (int k = 0; k < R.n; k++)
                if (!(R.v[k].style & 4) && !(R.v[k].style & 128)) R.v[k].style |= 1;
        int n = wrap_runs(&R, cw[j], fs, &C.L[j]);
        C.N[j] = n;
        if (n > C.maxl) C.maxl = n;
        free_runs(&R);
    }
    return C;
}

static void free_cells(Cells *C, int nc) {
    for (int j = 0; j < nc; j++) free_lines(C->L[j], C->N[j]);
    free(C->L); free(C->N);
}

static void draw_cells(Doc *d, Cells *C, int nc, const double *cw, double fs,
                       double pad, double lh, int ishdr) {
    double h = C->maxl * lh + 2 * pad;
    room(d, h);
    Buf *pg = CURPG(d);
    double ytop = d->y, x = ML;
    double total = 0;
    for (int j = 0; j < nc; j++) total += cw[j] + 2 * pad;
    if (ishdr)
        bprintf(pg, "0.92 0.92 0.92 rg %.2f %.2f %.2f %.2f f\n", ML, ytop - h, total, h);
    bprintf(pg, "0 0 0 rg 0.60 0.60 0.60 RG 0.55 w\n");
    for (int j = 0; j < nc; j++) {
        double wcell = cw[j] + 2 * pad;
        bprintf(pg, "%.2f %.2f %.2f %.2f re S\n", x, ytop - h, wcell, h);
        for (int k = 0; k < C->N[j]; k++) {
            double b = ytop - pad - fs * 0.86 - k * lh;
            Line2 *Ln = &C->L[j][k];

            /* Pass 1 */
            double curx = x + pad;
            for (int s = 0; s < Ln->n; s++) {
                Seg *g = &Ln->v[s];
                double sw = segw(g->txt, strlen(g->txt), g->style, fs);
                if (g->style & 64) {
                    render_emoji_seg(d, g, curx, b, fs, sw);
                    curx += sw;
                    continue;
                }
                if (g->url[0]) {
                    bprintf(pg, "0 0 1 RG 0.5 w %.2f %.2f m %.2f %.2f l S 0 0 0 RG\n",
                            curx, b - 1.5, curx + sw, b - 1.5);
                    add_link(d, curx, b - 2.0, curx + sw, b + fs * 0.8, g->url);
                }
                curx += sw;
            }

            /* Pass 2 con Tm por segmento */
            curx = x + pad;
            for (int s = 0; s < Ln->n; s++) {
                Seg *g = &Ln->v[s];
                double sw = segw(g->txt, strlen(g->txt), g->style, fs);
                if (g->style & 64) { curx += sw; continue; }
                bprintf(pg, "BT 1 0 0 1 %.2f %.2f Tm ", curx, b);
                if (g->url[0]) bputs(pg, "0 0 1 rg ");
                bprintf(pg, "/%s %.2f Tf (", font_of(g->style), fs);
                esc_pdf(pg, g->txt);
                bputs(pg, ") Tj 0 0 0 rg ET\n");
                curx += sw;
            }
        }
        x += wcell;
    }
    bprintf(pg, "0 0 0 RG\n");
    d->y -= h;
}

void render_table(Doc *d, Row *rows, int nr) {
    if (nr < 1) return;
    int nc = 0;
    for (int i = 0; i < nr; i++) if (rows[i].n > nc) nc = rows[i].n;
    if (nc < 1) return;
    for (int i = 0; i < nr; i++) {
        Row *r = &rows[i];
        while (r->n < nc) {
            if (r->n == r->cap) {
                r->cap = r->cap ? r->cap * 2 : 8;
                r->c = (char **)realloc(r->c, (size_t)r->cap * sizeof(char *));
            }
            r->c[r->n++] = xstrdup(" ");
        }
    }
    double fs = TF, pad = TPAD, lh = TF * 1.30;
    double avail = CONTENT_W - 2 * pad * nc;
    if (avail < nc * 12) avail = nc * 12;
    double *nat = (double *)calloc((size_t)nc, sizeof(double));
    for (int j = 0; j < nc; j++) {
        double mx = 0, mw = 0;
        for (int i = 0; i < nr; i++) {
            double w = textw(rows[i].c[j], fs, 0); if (w > mx) mx = w;
            w = longest_word(rows[i].c[j], fs); if (w > mw) mw = w;
        }
        double up = mx < avail * 0.50 ? mx : avail * 0.50;
        double lo = mw + 4;
        nat[j] = lo > up ? lo : up;
        if (nat[j] < 18) nat[j] = 18;
    }
    double sum = 0;
    for (int j = 0; j < nc; j++) sum += nat[j];
    double *cw = (double *)malloc((size_t)nc * sizeof(double));
    if (sum <= avail) {
        double extra = avail - sum;
        for (int j = 0; j < nc; j++) cw[j] = nat[j] + (sum > 0 ? extra * nat[j] / sum : 0);
    } else {
        char *act = (char *)malloc((size_t)nc); memset(act, 1, (size_t)nc);
        double remw = avail, remnat = sum;
        for (int it = 0; it < 10; it++) {
            int ch = 0;
            for (int j = 0; j < nc; j++) if (act[j]) {
                double w = remnat > 0 ? remw * nat[j] / remnat : MINW;
                if (w < MINW) {
                    cw[j] = MINW; act[j] = 0; remw -= MINW; remnat -= nat[j]; ch = 1;
                }
            }
            if (!ch) break;
        }
        for (int j = 0; j < nc; j++) if (act[j])
            cw[j] = remnat > 0 ? remw * nat[j] / remnat : MINW;
        free(act);
    }
    d->y -= 6;
    int hashdr = nr > 1;
    Cells H = prep_cells(&rows[0], nc, cw, fs, hashdr);
    draw_cells(d, &H, nc, cw, fs, pad, lh, hashdr);
    for (int i = 1; i < nr; i++) {
        Cells C = prep_cells(&rows[i], nc, cw, fs, 0);
        double h = C.maxl * lh + 2 * pad;
        if (d->y - h < MB) {
            new_page(d);
            draw_cells(d, &H, nc, cw, fs, pad, lh, hashdr);
        }
        draw_cells(d, &C, nc, cw, fs, pad, lh, 0);
        free_cells(&C, nc);
    }
    free_cells(&H, nc);
    d->y -= 8;
    free(nat); free(cw);
}

char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

int blank(const char *s) {
    for (; *s; s++) if (*s != ' ' && *s != '\t' && *s != '\r') return 0;
    return 1;
}

int is_hr(const char *s) {
    char c = 0; int n = 0;
    for (const char *t = s; *t; t++) {
        if (*t == ' ') continue;
        if (*t == '-' || *t == '*' || *t == '_') { if (!c) c = *t; if (*t != c) return 0; n++; }
        else return 0;
    }
    return n >= 3;
}

int is_table_line(const char *s) {
    while (*s == ' ') s++;
    return *s == '|';
}

void flush_para(Doc *d, Buf *para, int *have) {
    if (!*have) return;
    para_text(d, para->d);
    *have = 0;
    if (para->d) {
        free(para->d); 
    }
    binit(para);  
}


static int ci_match(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
    }
    return 1;
}

static const char *attr_value(const char *tag, const char *name) {
    size_t nl = strlen(name);
    const char *p = tag;
    while (*p && *p != '>') {
        int boundary = (p == tag || p[-1] == ' ' || p[-1] == '\t' ||
                        p[-1] == '\n' || p[-1] == '\r');
        if (boundary && ci_match(p, name, nl)) {
            const char *q = p + nl;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                return q;
            }
        }
        p++;
    }
    return NULL;
}

static int parse_attr_to_buf(const char *v, char *url, size_t usz) {
    if (!v) return 0;
    char q = 0;
    if (*v == '"' || *v == '\'') { q = *v; v++; }
    size_t i = 0;
    if (q) {
        while (v[i] && v[i] != q && i + 1 < usz) { url[i] = v[i]; i++; }
    } else {
        while (v[i] && v[i] != ' ' && v[i] != '\t' && v[i] != '>' && i + 1 < usz) { url[i] = v[i]; i++; }
    }
    url[i] = 0;
    return i > 0;
}

static void parse_width_value(const char *v, int *wpct, int *wpx) {
    if (!v) return;
    if (*v == '"' || *v == '\'') v++;
    while (*v == ' ' || *v == '\t') v++;
    int val = 0, dig = 0;
    while (*v >= '0' && *v <= '9') {
        val = val * 10 + (*v - '0');
        v++;
        dig = 1;
    }
    if (!dig) return;
    if (*v == '%') {
        if (val > 100) val = 100;
        *wpct = val;
    } else {
        if (val > 0) *wpx = val;
    }
}

int image_url(const char *t, char *url, size_t usz, int *wpct, int *wpx) {
    *wpct = 0;
    *wpx = 0;
    while (*t == ' ' || *t == '\t') t++;

    if (t[0] == '!' && t[1] == '[') {
        const char *cl = strchr(t + 2, ']');
        if (cl && cl[1] == '(') {
            int depth = 1;
            const char *cp = cl + 2;
            while (*cp && depth > 0) {
                if (*cp == '(') depth++;
                else if (*cp == ')') depth--;
                if (depth > 0) cp++;
            }
            if (depth == 0) {
                size_t ul = (size_t)(cp - cl - 2);
                if (ul && ul < usz) {
                    memcpy(url, cl + 2, ul);
                    url[ul] = 0;
                    char *sp = strchr(url, ' ');
                    if (sp) *sp = 0;
                    
                    /* ======================================================= */
                    /* TRUCO: Si es un badge de shields.io, NO lo trates como  */
                    /* una imagen de bloque independiente (vertical grande).   */
                    /* Devolvemos 0 para obligar a procesarlo de forma inline. */
                    /* ======================================================= */
                    if (strstr(url, "img.shields.io") || strstr(url, "badge")) {
                        url[0] = 0; // Limpiamos el buffer
                        return 0;   // Falso bloque -> Pasa al flujo horizontal
                    }
                    
                    return 1;
                }
            }
        }
        return 0;
    }
    
    if (ci_match(t, "<img", 4) && (t[4] == ' ' || t[4] == '\t' || t[4] == '>')) {
        const char *sv = attr_value(t, "src");
        if (sv) parse_attr_to_buf(sv, url, usz);
        const char *wv = attr_value(t, "width");
        parse_width_value(wv, wpct, wpx);
        
        /* Aplicamos el mismo filtro si el badge viene en formato HTML <img> */
        if (url[0] && (strstr(url, "img.shields.io") || strstr(url, "badge"))) {
            url[0] = 0;
            return 0;
        }
        
        if (url[0]) return 1;
    }
    return 0;
}

