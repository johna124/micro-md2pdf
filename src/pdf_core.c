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

Img *imgs = NULL;
int nimgs = 0, capimgs = 0;
Use *uses = NULL;
int nuses = 0, capuses = 0;
Link *links = NULL;
int nlinks = 0, caplinks = 0;

static const short WA[96] = {
    278,278,355,556,556,889,667,191,333,333,389,584,278,333,278,278,
    556,556,556,556,556,556,556,556,556,556,278,278,584,584,584,556,
    1015,667,667,722,722,667,611,778,722,278,500,667,556,833,722,778,
    667,778,722,667,611,722,667,944,667,667,611,278,278,278,469,556,
    333,556,556,500,556,556,278,556,556,222,222,500,222,833,556,556,
    556,556,333,500,278,556,500,722,500,500,500,334,260,334,584
};
static const short WX[32] = {
    556,556,222,556,333,1000,556,556,333,1000,667,333,1000,556,611,556,
    556,222,222,333,333,350,556,1000,333,1000,500,333,944,556,500,667
};
static const short WL[96] = {
    278,333,556,556,556,556,260,556,333,737,370,556,584,333,737,333,
    400,584,333,333,333,556,537,278,333,333,360,556,834,834,834,611,
    667,667,667,667,667,667,1000,722,667,667,667,667,278,278,278,278,
    722,722,778,778,778,778,778,584,778,722,722,722,722,667,667,611,
    556,556,556,556,556,556,889,500,556,556,556,556,278,278,278,278,
    556,556,556,556,556,556,556,584,611,556,556,556,556,500,556,500
};

int glyph_w(unsigned char u, int mono, int style) {
    int width;

    if (mono) return 600;
    
    if (u < 32)        width = 278;
    else if (u < 128)  width = WA[u - 32];
    else if (u < 0xA0) width = WX[u - 0x80];
    else               width = WL[u - 0xA0];

    if (style & 1) {
        return (int)(width * 1.055); 
    }

    return width;
}


double textw(const char *s, double fs, int mono) {
    double w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    w += glyph_w(*p, mono, 0); 
    return w / 1000.0 * fs;
}

void binit(Buf *b) { b->d = NULL; b->n = b->cap = 0; }

void bneed(Buf *b, size_t k) {
    if (b->n + k + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + k + 1) nc *= 2;
        b->d = (char *)realloc(b->d, nc);
        if (!b->d) { fprintf(stderr, "without memory\n"); exit(1); }
        b->cap = nc;
    }
}

void bput(Buf *b, const void *s, size_t k) {
    bneed(b, k);
    memcpy(b->d + b->n, s, k);
    b->n += k;
    b->d[b->n] = 0;
}

void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }
void bputc(Buf *b, char c) { bput(b, &c, 1); }

void bprintf(Buf *b, const char *fmt, ...) {
    va_list ap, aq;
    char tmp[1024];
    int n;
    va_start(ap, fmt);
    va_copy(aq, ap);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n >= 0 && (size_t)n < sizeof tmp) {
        bput(b, tmp, (size_t)n);
        va_end(aq);
        return;
    }
    if (n < 0) n = 0;
    bneed(b, (size_t)n);
    vsnprintf(b->d + b->n, (size_t)n + 1, fmt, aq);
    va_end(aq);
    b->n += (size_t)n;
}

void esc_pdf(Buf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '(' || c == ')' || c == '\\') {
            bputc(b, '\\');
            bputc(b, (char)c);
        } else if (c < 0x20 || c >= 0x80) {
            char o[8];
            snprintf(o, sizeof o, "\\%03o", c);
            bputs(b, o);
        } else {
            bputc(b, (char)c);
        }
    }
}

void new_page(Doc *d) {
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->pg = (Buf **)realloc(d->pg, (size_t)d->cap * sizeof(Buf *));
    }
    Buf *b = (Buf *)malloc(sizeof *b);
    binit(b);
    d->pg[d->n++] = b;
    d->y = PH - MT;
}

void room(Doc *d, double h) {
    if (d->y - h < MB) new_page(d);
}

void use_img(Doc *d, int id) {
    int pg = d->n - 1;
    for (int i = 0; i < nuses; i++)
        if (uses[i].page == pg && uses[i].img == id) return;
    if (nuses == capuses) {
        capuses = capuses ? capuses * 2 : 16;
        uses = (Use *)realloc(uses, (size_t)capuses * sizeof(Use));
    }
    uses[nuses].page = pg;
    uses[nuses].img = id;
    nuses++;
}

void add_link(Doc *d, double x1, double y1, double x2, double y2, const char *url) {
    if (nlinks == caplinks) {
        caplinks = caplinks ? caplinks * 2 : 16;
        links = (Link *)realloc(links, (size_t)caplinks * sizeof(Link));
    }
    Link *l = &links[nlinks++];
    l->page = d->n - 1;
    l->x1 = x1; l->y1 = y1;
    l->x2 = x2; l->y2 = y2;
    strncpy(l->url, url, sizeof(l->url) - 1);
    l->url[sizeof(l->url) - 1] = 0;
}

static long wbuf(FILE *f, const void *s, size_t n) { return (long)fwrite(s, 1, n, f); }
static long wstr(FILE *f, const char *s) { return wbuf(f, s, strlen(s)); }

int write_pdf(Doc *d, const char *path) {
    for (int i = 0; i < d->n; i++) {
        Buf *b = d->pg[i];
        char tmp[48];
        snprintf(tmp, sizeof tmp, "P\341gina %d de %d", i + 1, d->n);
        double w = textw(tmp, 8, 0);
        Buf fb = {0};
        bputc(&fb, '(');
        esc_pdf(&fb, tmp);
        bputc(&fb, ')');
        bprintf(b, "0.45 0.45 0.45 rg BT /F1 8 Tf %.2f 34 Td %s Tj ET 0 0 0 rg\n",
                (PW - w) / 2.0, fb.d ? fb.d : "()");
        free(fb.d);
    }

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    int np = d->n, ni = nimgs;
    int base = 7 + ni;
    int link_base = base + 2 * np;
    int total = link_base + nlinks - 1;
    long *off = (long *)malloc((size_t)(total + 1) * sizeof(long));

    wstr(f, "%PDF-1.4\n%\342\343\317\323\n");
    off[1] = ftell(f);
    wstr(f, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    Buf kids = {0};
    for (int i = 0; i < np; i++) bprintf(&kids, "%d 0 R ", base + 2 * i + 1);
    off[2] = ftell(f);
    fprintf(f, "2 0 obj\n<< /Type /Pages /Kids [%s] /Count %d >>\nendobj\n", kids.d, np);

    static const char *FN[4] = { "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Courier" };
    for (int k = 0; k < 4; k++) {
        off[3 + k] = ftell(f);
        fprintf(f, "%d 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /%s /Encoding /WinAnsiEncoding >>\nendobj\n",
                3 + k, FN[k]);
    }

    for (int k = 0; k < ni; k++) {
        Img *im = &imgs[k];
        off[7 + k] = ftell(f);
        if (im->jpeg) {
            fprintf(f, "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
                    "/ColorSpace %s /BitsPerComponent 8 /Filter /DCTDecode /Length %zu >>\nstream\n",
                    7 + k, im->rw, im->rh,
                    im->cs == 0 ? "/DeviceGray" : "/DeviceRGB",
                    im->len);
            wbuf(f, im->data, im->len);
     } else if (im->flate) {
        int smask_ref = -1;
        if (im->smask_id >= 0 && im->smask_id < ni) {
            smask_ref = 7 + im->smask_id;
        }
        if (smask_ref >= 0) {
            fprintf(f, "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
                    "/ColorSpace %s /BitsPerComponent 8 /Filter /FlateDecode /Length %zu /SMask %d 0 R >>\nstream\n",
                    7 + k, im->rw, im->rh,
                    im->cs == 0 ? "/DeviceGray" : "/DeviceRGB",
                    im->zlen, smask_ref);
        } else {
            fprintf(f, "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
                    "/ColorSpace %s /BitsPerComponent 8 /Filter /FlateDecode /Length %zu >>\nstream\n",
                    7 + k, im->rw, im->rh,
                    im->cs == 0 ? "/DeviceGray" : "/DeviceRGB",
                    im->zlen);
        }
        wbuf(f, im->zdata, im->zlen);
        } else {
            fprintf(f, "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
                    "/ColorSpace %s /BitsPerComponent 8 /Length %zu >>\nstream\n",
                    7 + k, im->rw, im->rh,
                    im->cs == 0 ? "/DeviceGray" : "/DeviceRGB",
                    im->len);
            wbuf(f, im->data, im->len);
        }
        wstr(f, "\nendstream\nendobj\n");
    }

    for (int i = 0; i < np; i++) {
        int cnum = base + 2 * i, pnum = base + 2 * i + 1;
        Buf *b = d->pg[i];

        off[cnum] = ftell(f);
        fprintf(f, "%d 0 obj\n<< /Length %zu >>\nstream\n", cnum, b->n);
        wbuf(f, b->d, b->n);
        wstr(f, "\nendstream\nendobj\n");

        Buf xo = {0};
        int any = 0;
        for (int u = 0; u < nuses; u++) {
            if (uses[u].page == i) {
                if (!any) bputs(&xo, " /XObject << ");
                bprintf(&xo, "/Im%d %d 0 R ", uses[u].img, 7 + uses[u].img);
                any = 1;
            }
        }
        if (any) bputs(&xo, ">> ");

        Buf annots = {0};
        int any_link = 0;
        for (int u = 0; u < nlinks; u++) {
            if (links[u].page == i) {
                if (!any_link) bputs(&annots, " /Annots [ ");
                bprintf(&annots, "%d 0 R ", link_base + u);
                any_link = 1;
            }
        }
        if (any_link) bputs(&annots, "] ");

        off[pnum] = ftell(f);
        fprintf(f, "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595.28 841.89] "
                "/Resources << /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R /F4 6 0 R >>%s >> "
                "/Contents %d 0 R%s >>\nendobj\n",
                pnum, xo.d ? xo.d : "", cnum, annots.d ? annots.d : "");
        free(xo.d);
        free(annots.d);
    }

    for (int u = 0; u < nlinks; u++) {
        off[link_base + u] = ftell(f);
        Buf urlbuf = {0};
        for (const char *p = links[u].url; *p; p++) {
            if (*p == '(' || *p == ')' || *p == '\\') bputc(&urlbuf, '\\');
            bputc(&urlbuf, *p);
        }
        fprintf(f, "%d 0 obj\n<< /Type /Annot /Subtype /Link /Rect [%.2f %.2f %.2f %.2f] "
                "/Border [0 0 0] /A << /Type /Action /S /URI /URI (%s) >> >>\nendobj\n",
                link_base + u,
                links[u].x1, links[u].y1, links[u].x2, links[u].y2,
                urlbuf.d ? urlbuf.d : "");
        free(urlbuf.d);
    }

    long xr = ftell(f);
    fprintf(f, "xref\n0 %d\n", total + 1);
    wstr(f, "0000000000 65535 f \n");
    for (int i = 1; i <= total; i++) fprintf(f, "%010ld 00000 n \n", off[i]);
    fprintf(f, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", total + 1, xr);

    fclose(f);
    free(off);
    free(kids.d);
    return np;
}
