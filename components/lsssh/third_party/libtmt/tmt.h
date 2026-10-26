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

/* LakeShark copy of libtmt (github.com/deadpixi/libtmt @ 1da7ba9), MODIFIED.
 * Changes from upstream, all so that a remote shell with TERM=xterm draws
 * correctly on a board whose C library has no UTF-8 locale:
 *   - UTF-8 is decoded by a small decoder in tmt.c; mbrtowc/wchar_t are gone
 *     (newlib in the "C" locale decodes nothing). A cell holds a 32-bit
 *     codepoint; 0 marks the right half of a double-width character.
 *   - The parser is restructured: OSC/DCS strings (window titles) are
 *     swallowed, unknown CSI/ESC finals are dropped instead of printed,
 *     private-mode (?) sequences are told apart, C0 controls act inside a
 *     sequence as on a VT100.
 *   - Added: scroll regions (DECSTBM), deferred wrap at the last column,
 *     IND/NEL/RI, DEC special graphics via ESC ( 0, application cursor
 *     keys (?1), the alternate screen (?47/?1047/?1049), bright and 256-
 *     colour/true-colour SGR folded to 16 colours, background colour erase.
 *   - Fixed: cursor-up/left underflow, ICH/ECH/EL1/ED1 ranges, SGR flags
 *     reading the first parameter, a double free on a failed resize.
 *   - Cells are 8 bytes (packed attributes) and every allocation goes
 *     through TMT_REALLOC/TMT_FREE so the board can put the grid in PSRAM.
 *   - tmt_open() no longer takes an ACS table.
 */

#ifndef TMT_H
#define TMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TMT_INVALID_CHAR ((uint32_t)0xfffd)

/**** BASIC DATA STRUCTURES */
typedef struct TMT TMT;

typedef enum{
    TMT_COLOR_DEFAULT = -1,
    TMT_COLOR_BLACK = 1,
    TMT_COLOR_RED,
    TMT_COLOR_GREEN,
    TMT_COLOR_YELLOW,
    TMT_COLOR_BLUE,
    TMT_COLOR_MAGENTA,
    TMT_COLOR_CYAN,
    TMT_COLOR_WHITE,
    TMT_COLOR_BRIGHT_BLACK,     /* 9..16: SGR 90-97 / 100-107 */
    TMT_COLOR_BRIGHT_WHITE = TMT_COLOR_BRIGHT_BLACK + 7,
    TMT_COLOR_MAX
} tmt_color_t;

typedef struct TMTATTRS TMTATTRS;
struct TMTATTRS{
    unsigned bold : 1;
    unsigned dim : 1;
    unsigned underline : 1;
    unsigned blink : 1;
    unsigned reverse : 1;
    unsigned invisible : 1;
    int8_t fg;                  /* tmt_color_t */
    int8_t bg;
};

typedef struct TMTCHAR TMTCHAR;
struct TMTCHAR{
    uint32_t c;                 /* codepoint; 0 = right half of a wide one */
    TMTATTRS a;
};

typedef struct TMTPOINT TMTPOINT;
struct TMTPOINT{
    size_t r;
    size_t c;
};

typedef struct TMTLINE TMTLINE;
struct TMTLINE{
    bool dirty;
    TMTCHAR chars[];
};

typedef struct TMTSCREEN TMTSCREEN;
struct TMTSCREEN{
    size_t nline;
    size_t ncol;

    TMTLINE **lines;
};

/**** CALLBACK SUPPORT */
typedef enum{
    TMT_MSG_MOVED,
    TMT_MSG_UPDATE,
    TMT_MSG_ANSWER,
    TMT_MSG_BELL,
    TMT_MSG_CURSOR
} tmt_msg_t;

typedef void (*TMTCALLBACK)(tmt_msg_t m, struct TMT *v, const void *r, void *p);

/**** PUBLIC FUNCTIONS */
TMT *tmt_open(size_t nline, size_t ncol, TMTCALLBACK cb, void *p);
void tmt_close(TMT *vt);
bool tmt_resize(TMT *vt, size_t nline, size_t ncol);
void tmt_write(TMT *vt, const char *s, size_t n);
const TMTSCREEN *tmt_screen(const TMT *vt);
const TMTPOINT *tmt_cursor(const TMT *vt);
void tmt_clean(TMT *vt);
void tmt_reset(TMT *vt);
/* LakeShark additions. */
bool tmt_cursor_visible(const TMT *vt);
bool tmt_app_cursor_keys(const TMT *vt);   /* DECCKM: arrows as ESC O x */

#endif
