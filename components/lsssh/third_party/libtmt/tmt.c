/* Copyright (c) 2017 Rob King
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the copyright holder nor the
 *     names of contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS,
 * COPYRIGHT HOLDERS, OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/* Modified for LakeShark; tmt.h lists the changes. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tmt.h"

#if defined(ESP_PLATFORM) && !defined(TMT_REALLOC)
/* The grid is the biggest thing a terminal owns; internal RAM is for drivers. */
#include "esp_heap_caps.h"
#define TMT_REALLOC(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define TMT_FREE(p)       heap_caps_free(p)
#endif
#ifndef TMT_REALLOC
#define TMT_REALLOC(p, n) realloc((p), (n))
#define TMT_FREE(p)       free(p)
#endif

#define BUF_MAX 100
#define PAR_MAX 16
#define TAB 8
#define MAX(x, y) (((size_t)(x) > (size_t)(y)) ? (size_t)(x) : (size_t)(y))
#define MIN(x, y) (((size_t)(x) < (size_t)(y)) ? (size_t)(x) : (size_t)(y))
#define CLINE(vt) (vt)->screen.lines[MIN((vt)->curs.r, (vt)->screen.nline - 1)]

#define P0(x) (vt->pars[x])
#define P1(x) (vt->pars[x]? vt->pars[x] : 1)
#define CB(vt, m, a) ((vt)->cb? (vt)->cb(m, vt, a, (vt)->p) : (void)0)

#define COMMON_VARS             \
    TMTSCREEN *s = &vt->screen; \
    TMTPOINT *c = &vt->curs;    \
    TMTLINE *l = CLINE(vt);     \
    TMTCHAR *t = vt->tabs->chars

#define HANDLER(name) static void name (TMT *vt) { COMMON_VARS; (void)s; (void)c; (void)l; (void)t;

struct TMT{
    TMTPOINT curs, oldcurs;
    TMTATTRS attrs, oldattrs;

    bool dirty, acs, oldacs;
    bool lcf;                 /* wrote the last column; wrap on the next char */
    bool decckm, cursor_on;
    size_t top, bot;          /* scroll region, inclusive */
    TMTSCREEN screen;
    TMTLINE *tabs;

    TMTCHAR *altsave;         /* the main screen while the alternate is up */
    bool inalt;

    TMTCALLBACK cb;
    void *p;

    uint32_t u8cp, u8min;     /* UTF-8 decoder */
    int u8need;
    uint32_t lastcp;          /* for REP */

    size_t pars[PAR_MAX];
    size_t npar;
    size_t arg;
    char priv;                /* '?' '>' '<' '=' after CSI, else 0 */
    char inter;               /* an intermediate byte, else 0 */
    char csel;                /* which charset ESC ( ) * + # % designates */
    enum {S_NUL, S_ESC, S_ARG, S_STR, S_STRESC, S_CHARSET} state;
};

static const TMTATTRS defattrs = {.fg = TMT_COLOR_DEFAULT, .bg = TMT_COLOR_DEFAULT};
static void writecharatcurs(TMT *vt, uint32_t w);

/* DEC special graphics, 0x5f..0x7e, as Unicode. */
static uint32_t
decgfx(unsigned char c)
{
    static const uint16_t map[] = {
        0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
        0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
        0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534,
        0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7};
    return (c >= 0x5f && c <= 0x7e)? map[c - 0x5f] : c;
}

static void
dirtylines(TMT *vt, size_t s, size_t e)
{
    vt->dirty = true;
    for (size_t i = s; i < e; i++)
        vt->screen.lines[i]->dirty = true;
}

/* Background colour erase: cleared cells take the current background. */
static void
clearline(TMT *vt, TMTLINE *l, size_t s, size_t e)
{
    TMTATTRS a = defattrs;
    a.bg = vt->attrs.bg;
    vt->dirty = l->dirty = true;
    for (size_t i = s; i < e; i++){     /* callers keep e within the line */
        l->chars[i].a = a;
        l->chars[i].c = ' ';
    }
}

static void
clearlines(TMT *vt, size_t r, size_t n)
{
    for (size_t i = r; i < r + n && i < vt->screen.nline; i++)
        clearline(vt, vt->screen.lines[i], 0, vt->screen.ncol);
}

/* Last line a scroll starting at r may touch: the region's bottom when r is
   inside it, the screen's otherwise. */
static size_t
scrend(const TMT *vt, size_t r)
{
    return (r >= vt->top && r <= vt->bot)? vt->bot : vt->screen.nline - 1;
}

static void
scrup(TMT *vt, size_t r, size_t n)
{
    size_t e = scrend(vt, r);
    if (r > e) return;
    size_t span = e - r + 1;
    n = MIN(n, span);
    if (!n) return;

    TMTLINE *buf[n];
    memcpy(buf, vt->screen.lines + r, n * sizeof(TMTLINE *));
    memmove(vt->screen.lines + r, vt->screen.lines + r + n,
            (span - n) * sizeof(TMTLINE *));
    memcpy(vt->screen.lines + e + 1 - n, buf, n * sizeof(TMTLINE *));
    clearlines(vt, e + 1 - n, n);
    dirtylines(vt, r, e + 1);
}

static void
scrdn(TMT *vt, size_t r, size_t n)
{
    size_t e = scrend(vt, r);
    if (r > e) return;
    size_t span = e - r + 1;
    n = MIN(n, span);
    if (!n) return;

    TMTLINE *buf[n];
    memcpy(buf, vt->screen.lines + e + 1 - n, n * sizeof(TMTLINE *));
    memmove(vt->screen.lines + r + n, vt->screen.lines + r,
            (span - n) * sizeof(TMTLINE *));
    memcpy(vt->screen.lines + r, buf, n * sizeof(TMTLINE *));
    clearlines(vt, r, n);
    dirtylines(vt, r, e + 1);
}

static void
linefeed(TMT *vt)
{
    if (vt->curs.r == vt->bot) scrup(vt, vt->top, 1);
    else if (vt->curs.r < vt->screen.nline - 1) vt->curs.r++;
}

HANDLER(ed)
    switch (P0(0)){
        case 0: clearline(vt, l, c->c, s->ncol);
                clearlines(vt, c->r + 1, s->nline - (c->r + 1));      break;
        case 1: clearlines(vt, 0, c->r);
                clearline(vt, l, 0, c->c + 1);                       break;
        case 2: case 3: clearlines(vt, 0, s->nline);                break;
        default:                                                     break;
    }
}

HANDLER(ich)
    size_t n = MIN(P1(0), s->ncol - c->c);
    memmove(l->chars + c->c + n, l->chars + c->c,
            (s->ncol - c->c - n) * sizeof(TMTCHAR));
    clearline(vt, l, c->c, c->c + n);
}

HANDLER(dch)
    size_t n = MIN(P1(0), s->ncol - c->c);
    memmove(l->chars + c->c, l->chars + c->c + n,
            (s->ncol - c->c - n) * sizeof(TMTCHAR));
    clearline(vt, l, s->ncol - n, s->ncol);
}

HANDLER(el)
    switch (P0(0)){
        case 0: clearline(vt, l, c->c, s->ncol);     break;
        case 1: clearline(vt, l, 0, c->c + 1);       break;
        case 2: clearline(vt, l, 0, s->ncol);        break;
    }
}

/* Nearest of the 16 colours for an RGB value, as tmt_color_t. */
static int8_t
rgb16(unsigned r, unsigned g, unsigned b)
{
    unsigned m = MAX(r, MAX(g, b));
    if (m < 48) return TMT_COLOR_BLACK;
    unsigned h = m / 2;
    int idx = (r > h? 1 : 0) | (g > h? 2 : 0) | (b > h? 4 : 0);
    if (idx == 7 && m < 112) return TMT_COLOR_BRIGHT_BLACK;   /* a grey */
    return (int8_t)((m > 175? TMT_COLOR_BRIGHT_BLACK : TMT_COLOR_BLACK) + idx);
}

static int8_t
color256(size_t i)
{
    if (i < 8)   return (int8_t)(TMT_COLOR_BLACK + i);
    if (i < 16)  return (int8_t)(TMT_COLOR_BRIGHT_BLACK + i - 8);
    if (i < 232){
        i -= 16;
        return rgb16((unsigned)(i / 36) * 51, (unsigned)(i / 6 % 6) * 51, (unsigned)(i % 6) * 51);
    }
    if (i < 256){
        unsigned v = 8 + 10 * (unsigned)(i - 232);
        return rgb16(v, v, v);
    }
    return TMT_COLOR_DEFAULT;
}

HANDLER(sgr)
    for (size_t i = 0; i < vt->npar; i++){
        size_t p = P0(i);
        int8_t *fgbg = (p >= 40 && p < 50) || p >= 100? &vt->attrs.bg : &vt->attrs.fg;
        switch (p){
            case  0: vt->attrs = defattrs;            break;
            case  1: vt->attrs.bold = 1;              break;
            case  2: vt->attrs.dim = 1;               break;
            case  4: vt->attrs.underline = 1;         break;
            case  5: vt->attrs.blink = 1;             break;
            case  7: vt->attrs.reverse = 1;           break;
            case  8: vt->attrs.invisible = 1;         break;
            case 10: vt->acs = false;                 break;
            case 11: vt->acs = true;                  break;
            case 21: case 22: vt->attrs.bold = vt->attrs.dim = 0; break;
            case 24: vt->attrs.underline = 0;         break;
            case 25: vt->attrs.blink = 0;             break;
            case 27: vt->attrs.reverse = 0;           break;
            case 28: vt->attrs.invisible = 0;         break;
            case 39: case 49: *fgbg = TMT_COLOR_DEFAULT; break;
            case 38: case 48:
                if (i + 2 < vt->npar && P0(i + 1) == 5){
                    *fgbg = color256(P0(i + 2));
                    i += 2;
                } else if (i + 4 < vt->npar && P0(i + 1) == 2){
                    *fgbg = rgb16((unsigned)MIN(P0(i + 2), 255), (unsigned)MIN(P0(i + 3), 255),
                                  (unsigned)MIN(P0(i + 4), 255));
                    i += 4;
                } else {
                    i = vt->npar;      /* malformed: drop the rest */
                }
                break;
            default:
                if (p >= 30 && p <= 37)        *fgbg = (int8_t)(TMT_COLOR_BLACK + p - 30);
                else if (p >= 40 && p <= 47)   *fgbg = (int8_t)(TMT_COLOR_BLACK + p - 40);
                else if (p >= 90 && p <= 97)   *fgbg = (int8_t)(TMT_COLOR_BRIGHT_BLACK + p - 90);
                else if (p >= 100 && p <= 107) *fgbg = (int8_t)(TMT_COLOR_BRIGHT_BLACK + p - 100);
                break;
        }
    }
}

HANDLER(rep)
    if (!vt->lastcp) return;
    for (size_t i = 0; i < MIN(P1(0), s->ncol * s->nline); i++)
        writecharatcurs(vt, vt->lastcp);
}

HANDLER(dsr)
    char r[BUF_MAX + 1] = {0};
    snprintf(r, BUF_MAX, "\033[%u;%uR", (unsigned)(c->r + 1), (unsigned)(c->c + 1));
    CB(vt, TMT_MSG_ANSWER, (const char *)r);
}

static void
resetparser(TMT *vt)
{
    memset(vt->pars, 0, sizeof(vt->pars));
    vt->npar = vt->arg = 0;
    vt->priv = vt->inter = 0;
    vt->state = S_NUL;
}

static void
consumearg(TMT *vt)
{
    if (vt->npar < PAR_MAX)
        vt->pars[vt->npar++] = vt->arg;
    vt->arg = 0;
}

HANDLER(fixcursor)
    c->r = MIN(c->r, s->nline - 1);
    c->c = MIN(c->c, s->ncol - 1);
}

static void
savecursor(TMT *vt)
{
    vt->oldcurs = vt->curs;
    vt->oldattrs = vt->attrs;
    vt->oldacs = vt->acs;
}

static void
restorecursor(TMT *vt)
{
    vt->curs = vt->oldcurs;
    vt->attrs = vt->oldattrs;
    vt->acs = vt->oldacs;
    vt->lcf = false;
    fixcursor(vt);
}

/* ?47 / ?1047 / ?1049. The main screen is copied aside rather than kept as
   a second set of lines, so a resize just drops the copy. */
static void
altscreen(TMT *vt, bool on, bool cursor)
{
    TMTSCREEN *s = &vt->screen;
    size_t row = s->ncol * sizeof(TMTCHAR);
    if (on && !vt->inalt){
        if (cursor) savecursor(vt);
        vt->altsave = TMT_REALLOC(NULL, s->nline * row);
        if (vt->altsave)
            for (size_t i = 0; i < s->nline; i++)
                memcpy(vt->altsave + i * s->ncol, s->lines[i]->chars, row);
        vt->inalt = true;
        clearlines(vt, 0, s->nline);
    } else if (!on && vt->inalt){
        if (vt->altsave){
            for (size_t i = 0; i < s->nline; i++)
                memcpy(s->lines[i]->chars, vt->altsave + i * s->ncol, row);
            TMT_FREE(vt->altsave);
            vt->altsave = NULL;
        } else {
            clearlines(vt, 0, s->nline);
        }
        vt->inalt = false;
        if (cursor) restorecursor(vt);
        dirtylines(vt, 0, s->nline);
    }
}

static void
decset(TMT *vt, size_t mode, bool on)
{
    switch (mode){
        case 1:    vt->decckm = on;                                      break;
        case 25:   vt->cursor_on = on; CB(vt, TMT_MSG_CURSOR, on? "t" : "f"); break;
        case 47: case 1047: altscreen(vt, on, false);                    break;
        case 1049: altscreen(vt, on, true);                              break;
        default:                                                         break;
    }
}

static void
tabforward(TMT *vt, size_t n)
{
    TMTCHAR *t = vt->tabs->chars;
    /* Upstream stepped past the last column when already on it. */
    while (n-- && vt->curs.c < vt->screen.ncol - 1)
        while (++vt->curs.c < vt->screen.ncol - 1 && t[vt->curs.c].c != '*') ;
}

static void
csi(TMT *vt, char f)
{
    COMMON_VARS;
    size_t n = P1(0);

    if (f != 'm') vt->lcf = false;
    if (vt->inter) return;               /* CSI SP q and friends: nothing to draw */
    if (vt->priv == '?'){
        if (f == 'h' || f == 'l')
            for (size_t i = 0; i < vt->npar; i++) decset(vt, P0(i), f == 'h');
        return;
    }
    if (vt->priv) return;                /* CSI > c (secondary DA) and the like */

    switch (f){
        case 'A': c->r = c->r > n? c->r - n : 0;                break;
        case 'B': case 'e': c->r = MIN(c->r + n, s->nline - 1); break;
        case 'C': case 'a': c->c = MIN(c->c + n, s->ncol - 1);  break;
        case 'D': c->c = c->c > n? c->c - n : 0;                break;
        case 'E': c->c = 0; c->r = MIN(c->r + n, s->nline - 1); break;
        case 'F': c->c = 0; c->r = c->r > n? c->r - n : 0;      break;
        case 'G': case '`': c->c = MIN(n - 1, s->ncol - 1);     break;
        case 'd': c->r = MIN(n - 1, s->nline - 1);              break;
        case 'H': case 'f': c->r = P1(0) - 1; c->c = P1(1) - 1; break;
        case 'I': tabforward(vt, n);                            break;
        case 'Z': while (n--) while (c->c && t[--c->c].c != '*') ; break;
        case 'J': ed(vt);                                       break;
        case 'K': el(vt);                                       break;
        case 'L': if (c->r >= vt->top && c->r <= vt->bot) scrdn(vt, c->r, n); break;
        case 'M': if (c->r >= vt->top && c->r <= vt->bot) scrup(vt, c->r, n); break;
        case 'P': dch(vt);                                      break;
        case '@': ich(vt);                                      break;
        case 'X': clearline(vt, l, c->c, MIN(c->c + n, s->ncol)); break;
        case 'S': scrup(vt, vt->top, n);                        break;
        case 'T': scrdn(vt, vt->top, n);                        break;
        case 'b': rep(vt);                                      break;
        case 'c': if (!P0(0)) CB(vt, TMT_MSG_ANSWER, "\033[?6c"); break;
        case 'g': if (P0(0) == 3) clearline(vt, vt->tabs, 0, s->ncol);
                  else if (!P0(0)) t[c->c].c = ' ';
                  break;
        case 'm': sgr(vt);                                      break;
        case 'n': if (P0(0) == 6) dsr(vt);
                  else if (P0(0) == 5) CB(vt, TMT_MSG_ANSWER, "\033[0n");
                  break;
        case 'r': {
            size_t top = P1(0) - 1;
            size_t bot = (vt->npar > 1 && P0(1))? P0(1) - 1 : s->nline - 1;
            bot = MIN(bot, s->nline - 1);
            if (top < bot){ vt->top = top; vt->bot = bot; }
            c->r = c->c = 0;
            break;
        }
        case 's': savecursor(vt);                               break;
        case 'u': restorecursor(vt);                            break;
        default:                                                break;
    }
}

/* True when the byte was a control or part of a sequence. */
static bool
handlechar(TMT *vt, unsigned char i)
{
    TMTPOINT *c = &vt->curs;

    /* OSC, DCS, PM, APC: swallowed to BEL or ST. A title may be UTF-8. */
    if (vt->state == S_STR){
        if (i == 0x07 || i == 0x18 || i == 0x1a) resetparser(vt);
        else if (i == 0x1b) vt->state = S_STRESC;
        return true;
    }
    if (vt->state == S_STRESC){
        if (i == '\\') resetparser(vt);
        else vt->state = S_STR;
        return true;
    }

    /* C0 controls act wherever they are, even inside a sequence. */
    if (i < 0x20 || i == 0x7f){
        switch (i){
            case 0x07: CB(vt, TMT_MSG_BELL, NULL);                      break;
            case 0x08: vt->lcf = false; if (c->c) c->c--;               break;
            case 0x09: vt->lcf = false; tabforward(vt, 1);              break;
            case 0x0a: case 0x0b: case 0x0c: vt->lcf = false; linefeed(vt); break;
            case 0x0d: vt->lcf = false; c->c = 0;                       break;
            case 0x18: case 0x1a: resetparser(vt);                      break;
            case 0x1b: resetparser(vt); vt->state = S_ESC;              break;
            default:                                                    break;
        }
        return true;
    }

    if (vt->state == S_NUL) return false;
    if (i >= 0x80){                      /* a sequence broken by text */
        resetparser(vt);
        return false;
    }

    switch (vt->state){
        case S_CHARSET:
            if (vt->csel == '(') vt->acs = (i == '0');
            resetparser(vt);
            return true;

        case S_ESC:
            switch (i){
                case '[': vt->state = S_ARG;                     return true;
                case ']': case 'P': case 'X': case '^': case '_':
                          vt->state = S_STR;                     return true;
                case '(': case ')': case '*': case '+': case '#': case '%': case ' ':
                          vt->csel = (char)i; vt->state = S_CHARSET; return true;
                case '7': savecursor(vt);                        break;
                case '8': restorecursor(vt);                     break;
                case 'c': tmt_reset(vt);                         break;
                case 'D': vt->lcf = false; linefeed(vt);         break;
                case 'E': vt->lcf = false; c->c = 0; linefeed(vt); break;
                case 'M': vt->lcf = false;
                          if (c->r == vt->top) scrdn(vt, vt->top, 1);
                          else if (c->r) c->r--;
                          break;
                case 'H': vt->tabs->chars[c->c].c = '*';         break;
                default:  /* ESC = / ESC > keypad modes and the rest */ break;
            }
            resetparser(vt);
            return true;

        case S_ARG:
            if (i >= '0' && i <= '9'){
                if (vt->arg < 100000) vt->arg = vt->arg * 10 + (i - '0');
                return true;
            }
            if (i == ';' || i == ':'){ consumearg(vt); return true; }
            if (i >= 0x3c && i <= 0x3f){ vt->priv = (char)i; return true; }
            if (i >= 0x20 && i <= 0x2f){ vt->inter = (char)i; return true; }
            consumearg(vt);
            csi(vt, (char)i);
            fixcursor(vt);
            resetparser(vt);
            return true;

        default:
            resetparser(vt);
            return true;
    }
}

static void
notify(TMT *vt, bool update, bool moved)
{
    if (update) CB(vt, TMT_MSG_UPDATE, &vt->screen);
    if (moved) CB(vt, TMT_MSG_MOVED, &vt->curs);
}

static TMTLINE *
allocline(TMT *vt, TMTLINE *o, size_t n, size_t pc)
{
    TMTLINE *l = TMT_REALLOC(o, sizeof(TMTLINE) + n * sizeof(TMTCHAR));
    if (!l) return NULL;

    clearline(vt, l, pc, n);
    return l;
}

static void
freelines(TMT *vt, size_t s, size_t n, bool screen)
{
    for (size_t i = s; vt->screen.lines && i < s + n; i++){
        TMT_FREE(vt->screen.lines[i]);
        vt->screen.lines[i] = NULL;
    }
    if (screen){ TMT_FREE(vt->screen.lines); vt->screen.lines = NULL; }
}

TMT *
tmt_open(size_t nline, size_t ncol, TMTCALLBACK cb, void *p)
{
    TMT *vt = TMT_REALLOC(NULL, sizeof(TMT));
    if (!vt) return NULL;
    memset(vt, 0, sizeof(*vt));
    if (!nline || !ncol) return TMT_FREE(vt), NULL;

    vt->attrs = vt->oldattrs = defattrs;
    vt->cursor_on = true;
    vt->cb = cb;
    vt->p = p;

    if (!tmt_resize(vt, nline, ncol)) return tmt_close(vt), NULL;
    return vt;
}

void
tmt_close(TMT *vt)
{
    if (!vt) return;
    TMT_FREE(vt->tabs);
    TMT_FREE(vt->altsave);
    freelines(vt, 0, vt->screen.nline, true);
    TMT_FREE(vt);
}

bool
tmt_resize(TMT *vt, size_t nline, size_t ncol)
{
    if (nline < 2 || ncol < 2) return false;
    if (nline < vt->screen.nline){
        freelines(vt, nline, vt->screen.nline - nline, false);
        vt->screen.nline = nline;
    }

    TMTLINE **l = TMT_REALLOC(vt->screen.lines, nline * sizeof(TMTLINE *));
    if (!l) return false;

    size_t pc = vt->screen.ncol;
    vt->screen.lines = l;
    for (size_t i = vt->screen.nline; i < nline; i++) l[i] = NULL;
    for (size_t i = 0; i < nline; i++){
        TMTLINE *nl = allocline(vt, l[i], ncol, i < vt->screen.nline? MIN(pc, ncol) : 0);
        if (!nl){
            /* Keep what was grown so tmt_close frees it all. */
            vt->screen.nline = MAX(vt->screen.nline, i);
            return false;
        }
        l[i] = nl;
    }
    vt->screen.nline = nline;
    vt->screen.ncol = ncol;

    TMTLINE *tabs = allocline(vt, vt->tabs, ncol, 0);
    if (!tabs) return false;
    vt->tabs = tabs;
    vt->tabs->chars[0].c = vt->tabs->chars[ncol - 1].c = '*';
    for (size_t i = 0; i < ncol; i++) if (i % TAB == 0)
        vt->tabs->chars[i].c = '*';

    /* The saved main screen has the old shape: drop it. */
    TMT_FREE(vt->altsave);
    vt->altsave = NULL;
    vt->top = 0;
    vt->bot = nline - 1;
    vt->lcf = false;
    fixcursor(vt);
    dirtylines(vt, 0, nline);
    notify(vt, true, true);
    return true;
}

/* Display width of a codepoint: 0 for combining marks, 2 for the East Asian
   wide blocks and emoji, else 1. Coarse, and enough to keep columns
   aligned with what the remote side assumed. */
static int
cpwidth(uint32_t w)
{
    if ((w >= 0x0300 && w <= 0x036f) || (w >= 0x200b && w <= 0x200f) ||
        (w >= 0xfe00 && w <= 0xfe0f)) return 0;
    if ((w >= 0x1100 && w <= 0x115f) || (w >= 0x2e80 && w <= 0xa4cf && w != 0x303f) ||
        (w >= 0xac00 && w <= 0xd7a3) || (w >= 0xf900 && w <= 0xfaff) ||
        (w >= 0xfe30 && w <= 0xfe4f) || (w >= 0xff00 && w <= 0xff60) ||
        (w >= 0xffe0 && w <= 0xffe6) || (w >= 0x1f300 && w <= 0x1f64f) ||
        (w >= 0x1f900 && w <= 0x1f9ff) || (w >= 0x20000 && w <= 0x3fffd)) return 2;
    return 1;
}

static void
putcell(TMT *vt, uint32_t w)
{
    COMMON_VARS;
    (void)t;
    if (vt->lcf){                        /* deferred wrap */
        vt->lcf = false;
        c->c = 0;
        linefeed(vt);
        l = CLINE(vt);
    }
    l->chars[c->c].c = w;
    l->chars[c->c].a = vt->attrs;
    l->dirty = vt->dirty = true;
    if (c->c < s->ncol - 1) c->c++;
    else vt->lcf = true;
}

static void
writecharatcurs(TMT *vt, uint32_t w)
{
    int width = cpwidth(w);
    if (!width) return;
    vt->lastcp = w;
    if (width == 2){
        if (vt->screen.ncol < 2){ putcell(vt, TMT_INVALID_CHAR); return; }
        /* A wide character never splits across the margin. */
        if (vt->curs.c == vt->screen.ncol - 1 && !vt->lcf){
            putcell(vt, ' ');
        }
        putcell(vt, w);
        putcell(vt, 0);
        return;
    }
    putcell(vt, w);
}

/* Self-contained UTF-8: newlib's mbrtowc in the "C" locale passes bytes
   through one at a time, which is no use for a remote shell. */
static void
decodebyte(TMT *vt, unsigned char b)
{
    if (vt->u8need){
        if ((b & 0xc0) == 0x80){
            vt->u8cp = (vt->u8cp << 6) | (b & 0x3f);
            if (--vt->u8need == 0){
                uint32_t cp = vt->u8cp;
                bool bad = cp < vt->u8min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff);
                writecharatcurs(vt, bad? TMT_INVALID_CHAR : cp);
            }
            return;
        }
        vt->u8need = 0;                  /* cut short: say so, then look at b afresh */
        writecharatcurs(vt, TMT_INVALID_CHAR);
    }

    if (handlechar(vt, b)) return;

    if (b < 0x80)                     writecharatcurs(vt, vt->acs? decgfx(b) : b);
    else if ((b & 0xe0) == 0xc0){ vt->u8cp = b & 0x1f; vt->u8need = 1; vt->u8min = 0x80; }
    else if ((b & 0xf0) == 0xe0){ vt->u8cp = b & 0x0f; vt->u8need = 2; vt->u8min = 0x800; }
    else if ((b & 0xf8) == 0xf0){ vt->u8cp = b & 0x07; vt->u8need = 3; vt->u8min = 0x10000; }
    else                              writecharatcurs(vt, TMT_INVALID_CHAR);
}

void
tmt_write(TMT *vt, const char *s, size_t n)
{
    TMTPOINT oc = vt->curs;
    n = n? n : strlen(s);

    for (size_t p = 0; p < n; p++)
        decodebyte(vt, (unsigned char)s[p]);

    notify(vt, vt->dirty, memcmp(&oc, &vt->curs, sizeof(oc)) != 0);
}

const TMTSCREEN *
tmt_screen(const TMT *vt)
{
    return &vt->screen;
}

const TMTPOINT *
tmt_cursor(const TMT *vt)
{
    return &vt->curs;
}

bool
tmt_cursor_visible(const TMT *vt)
{
    return vt->cursor_on;
}

bool
tmt_app_cursor_keys(const TMT *vt)
{
    return vt->decckm;
}

void
tmt_clean(TMT *vt)
{
    for (size_t i = 0; i < vt->screen.nline; i++)
        vt->dirty = vt->screen.lines[i]->dirty = false;
}

void
tmt_reset(TMT *vt)
{
    vt->curs.r = vt->curs.c = vt->oldcurs.r = vt->oldcurs.c = 0;
    vt->acs = vt->oldacs = vt->lcf = vt->decckm = false;
    vt->cursor_on = true;
    vt->u8need = 0;
    vt->lastcp = 0;
    resetparser(vt);
    vt->attrs = vt->oldattrs = defattrs;
    TMT_FREE(vt->altsave);
    vt->altsave = NULL;
    vt->inalt = false;
    vt->top = 0;
    vt->bot = vt->screen.nline - 1;
    clearlines(vt, 0, vt->screen.nline);
    CB(vt, TMT_MSG_CURSOR, "t");
    notify(vt, true, true);
}
