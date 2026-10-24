/* The SSH app's keyboard encoder and its terminal emulator (the vendored,
   modified libtmt): what a key sends, and what a few escape sequences leave
   on the grid. */

#include "ls_test.h"

#include <stdint.h>
#include <string.h>

#include "ls_ssh_keys.h"
#include "tmt.h"

static ls_ssh_key_action_t enc(ls_ssh_mods_t *m, ls_tk_t k, char ch, bool app, char *out, int *n)
{
    memset(out, 0, LS_SSH_KEY_MAX);
    return ls_ssh_key_encode(m, k, ch, app, out, n);
}

LS_CASE(keys_plain)
{
    ls_ssh_mods_t m = {0};
    char out[LS_SSH_KEY_MAX];
    int n;
    LS_EQ_INT(enc(&m, LS_TK_CHAR, 'a', false, out, &n), LS_SSH_KEY_SEND);
    LS_EQ_INT(n, 1); LS_EQ_INT(out[0], 'a');
    enc(&m, LS_TK_ENTER, 0, false, out, &n);     LS_EQ_INT(n, 1); LS_EQ_INT(out[0], '\r');
    enc(&m, LS_TK_BACKSPACE, 0, false, out, &n); LS_EQ_INT(n, 1); LS_EQ_INT(out[0], 0x7f);
    enc(&m, LS_TK_TAB, 0, false, out, &n);       LS_EQ_INT(out[0], '\t');
    enc(&m, LS_TK_ESC, 0, false, out, &n);       LS_EQ_INT(n, 1); LS_EQ_INT(out[0], 0x1b);
    enc(&m, LS_TK_UP, 0, false, out, &n);        LS_EQ_INT(n, 3); LS_EQ_STR(out, "\x1b[A");
    enc(&m, LS_TK_LEFT, 0, true, out, &n);       LS_EQ_STR(out, "\x1bOD");
    /* The real F-keys stay the router's. */
    LS_EQ_INT(enc(&m, LS_TK_F1, 0, false, out, &n), LS_SSH_KEY_PASS);
    LS_EQ_INT(enc(&m, LS_TK_F11, 0, false, out, &n), LS_SSH_KEY_PASS);
}

LS_CASE(keys_sticky_ctrl_alt)
{
    ls_ssh_mods_t m = {0};
    char out[LS_SSH_KEY_MAX];
    int n;
    LS_EQ_INT(enc(&m, LS_TK_CTRL, 0, false, out, &n), LS_SSH_KEY_HELD);
    LS_CHECK(m.ctrl);
    enc(&m, LS_TK_CHAR, 'c', false, out, &n);
    LS_EQ_INT(n, 1); LS_EQ_INT(out[0], 0x03);
    LS_CHECK(!m.ctrl);                                   /* one key only */
    enc(&m, LS_TK_CHAR, 'c', false, out, &n);  LS_EQ_INT(out[0], 'c');
    enc(&m, LS_TK_CTRL, 0, false, out, &n); enc(&m, LS_TK_CHAR, 'D', false, out, &n); LS_EQ_INT(out[0], 0x04);
    enc(&m, LS_TK_CTRL, 0, false, out, &n); enc(&m, LS_TK_CHAR, 'z', false, out, &n); LS_EQ_INT(out[0], 0x1a);
    enc(&m, LS_TK_CTRL, 0, false, out, &n); enc(&m, LS_TK_CHAR, 'l', false, out, &n); LS_EQ_INT(out[0], 0x0c);
    enc(&m, LS_TK_CTRL, 0, false, out, &n); enc(&m, LS_TK_CHAR, '[', false, out, &n); LS_EQ_INT(out[0], 0x1b);
    enc(&m, LS_TK_CTRL, 0, false, out, &n); enc(&m, LS_TK_CHAR, ' ', false, out, &n);
    LS_EQ_INT(n, 1); LS_EQ_INT(out[0], 0x00);
    /* Pressing CTRL twice cancels it. */
    enc(&m, LS_TK_CTRL, 0, false, out, &n); enc(&m, LS_TK_CTRL, 0, false, out, &n);
    LS_CHECK(!m.ctrl);
    enc(&m, LS_TK_CHAR, 'x', false, out, &n); LS_EQ_INT(out[0], 'x');
    /* ALT is an ESC prefix; with CTRL too, both apply. */
    enc(&m, LS_TK_ALT, 0, false, out, &n); enc(&m, LS_TK_CHAR, 'b', false, out, &n);
    LS_EQ_INT(n, 2); LS_EQ_STR(out, "\x1b" "b");
    enc(&m, LS_TK_ALT, 0, false, out, &n); enc(&m, LS_TK_CTRL, 0, false, out, &n);
    enc(&m, LS_TK_CHAR, 'a', false, out, &n);
    LS_EQ_INT(n, 2); LS_EQ_INT(out[0], 0x1b); LS_EQ_INT(out[1], 0x01);
    /* Modified arrows use xterm's CSI 1;m form. */
    enc(&m, LS_TK_CTRL, 0, false, out, &n); enc(&m, LS_TK_RIGHT, 0, false, out, &n);
    LS_EQ_STR(out, "\x1b[1;5C");
    LS_CHECK(!m.ctrl);
}

LS_CASE(keys_fn_layer)
{
    ls_ssh_mods_t m = {0};
    char out[LS_SSH_KEY_MAX];
    int n;
    enc(&m, LS_TK_FN, 0, false, out, &n);
    LS_EQ_INT(enc(&m, LS_TK_CHAR, 'q', false, out, &n), LS_SSH_KEY_DISCONNECT);
    LS_CHECK(!m.fn);
    enc(&m, LS_TK_FN, 0, false, out, &n); enc(&m, LS_TK_UP, 0, false, out, &n);   LS_EQ_STR(out, "\x1b[5~");
    enc(&m, LS_TK_FN, 0, false, out, &n); enc(&m, LS_TK_DOWN, 0, false, out, &n); LS_EQ_STR(out, "\x1b[6~");
    enc(&m, LS_TK_FN, 0, false, out, &n); enc(&m, LS_TK_BACKSPACE, 0, false, out, &n); LS_EQ_STR(out, "\x1b[3~");
    enc(&m, LS_TK_FN, 0, false, out, &n); enc(&m, LS_TK_CHAR, '1', false, out, &n); LS_EQ_STR(out, "\x1bOP");
    enc(&m, LS_TK_FN, 0, false, out, &n); enc(&m, LS_TK_CHAR, '0', false, out, &n); LS_EQ_STR(out, "\x1b[21~");
    /* FN with a key it has no meaning for is just that key. */
    enc(&m, LS_TK_FN, 0, false, out, &n);
    LS_EQ_INT(enc(&m, LS_TK_CHAR, 'w', false, out, &n), LS_SSH_KEY_SEND);
    LS_EQ_INT(n, 1); LS_EQ_INT(out[0], 'w');
    LS_CHECK(!m.fn);
    /* Plain q is just q. */
    enc(&m, LS_TK_CHAR, 'q', false, out, &n); LS_EQ_INT(out[0], 'q');
}

/* ------------------------------------------------------------- emulator */

static char s_answer[64];

static void cb(tmt_msg_t m, TMT *vt, const void *r, void *p)
{
    (void)vt; (void)p;
    if (m == TMT_MSG_ANSWER) snprintf(s_answer, sizeof(s_answer), "%s", (const char *)r);
}

/* Row y as ASCII, trailing spaces trimmed; '~' for anything else. */
static const char *row(TMT *vt, int y)
{
    static char buf[256];
    const TMTSCREEN *s = tmt_screen(vt);
    size_t n = 0;
    for (size_t x = 0; x < s->ncol && n < sizeof(buf) - 1; ++x) {
        uint32_t c = s->lines[y]->chars[x].c;
        buf[n++] = c >= 0x20 && c < 0x7f ? (char)c : c == 0 ? '_' : '~';
    }
    while (n && buf[n - 1] == ' ') --n;
    buf[n] = 0;
    return buf;
}

static void put(TMT *vt, const char *s) { tmt_write(vt, s, strlen(s)); }

LS_CASE(term_text_and_wrap)
{
    TMT *vt = tmt_open(4, 10, cb, NULL);
    LS_CHECK(vt != NULL);
    put(vt, "hello\r\nworld");
    LS_EQ_STR(row(vt, 0), "hello");
    LS_EQ_STR(row(vt, 1), "world");
    LS_EQ_INT(tmt_cursor(vt)->r, 1); LS_EQ_INT(tmt_cursor(vt)->c, 5);
    /* Exactly ten columns then CRLF: deferred wrap, so no blank line. */
    put(vt, "\r\n0123456789\r\nx");
    LS_EQ_STR(row(vt, 2), "0123456789");
    LS_EQ_STR(row(vt, 3), "x");
    /* One more line scrolls the top off. */
    put(vt, "\r\nlast");
    LS_EQ_STR(row(vt, 0), "world");
    LS_EQ_STR(row(vt, 3), "last");
    tmt_close(vt);
}

LS_CASE(term_csi_and_osc)
{
    TMT *vt = tmt_open(5, 20, cb, NULL);
    /* A bash prompt sets the window title first; none of it may show. */
    put(vt, "\x1b]0;user@host: ~\x07$ ");
    LS_EQ_STR(row(vt, 0), "$");
    /* Clear, home, position, erase to end of line. */
    put(vt, "\x1b[2J\x1b[H" "abcdef" "\x1b[1;3H" "\x1b[K");
    LS_EQ_STR(row(vt, 0), "ab");
    put(vt, "\x1b[3;5HX");
    LS_EQ_STR(row(vt, 2), "    X");
    /* Cursor up past the top stops at the top. */
    put(vt, "\x1b[99A");
    LS_EQ_INT(tmt_cursor(vt)->r, 0);
    /* Unknown finals are dropped, not printed. */
    put(vt, "\x1b[2J\x1b[H\x1b[23;0;0t\x1b[>c\x1b[?2004h\x1b=ok");
    LS_EQ_STR(row(vt, 0), "ok");
    /* Device status report is answered. */
    s_answer[0] = 0;
    put(vt, "\x1b[6n");
    LS_EQ_STR(s_answer, "\x1b[1;3R");
    tmt_close(vt);
}

LS_CASE(term_colours)
{
    TMT *vt = tmt_open(3, 20, cb, NULL);
    put(vt, "\x1b[1;31mR\x1b[0m\x1b[92mG\x1b[38;5;21mB\x1b[7mV\x1b[m.");
    const TMTCHAR *c = tmt_screen(vt)->lines[0]->chars;
    LS_EQ_INT(c[0].a.fg, TMT_COLOR_RED);  LS_CHECK(c[0].a.bold);
    LS_EQ_INT(c[1].a.fg, TMT_COLOR_BRIGHT_BLACK + 2);     /* bright green */
    LS_CHECK(!c[1].a.bold);
    LS_EQ_INT(c[2].a.fg, TMT_COLOR_BRIGHT_BLACK + 4);     /* 256-colour blue folds to bright blue */
    LS_CHECK(c[3].a.reverse);
    LS_EQ_INT(c[4].a.fg, TMT_COLOR_DEFAULT); LS_CHECK(!c[4].a.reverse);
    tmt_close(vt);
}

LS_CASE(term_utf8_and_graphics)
{
    TMT *vt = tmt_open(3, 20, cb, NULL);
    put(vt, "\xc3\xa9\xe2\x94\x80\xf0\x9f\x98\x80!");           /* e-acute, box line, emoji */
    const TMTCHAR *c = tmt_screen(vt)->lines[0]->chars;
    LS_EQ_UINT(c[0].c, 0xe9);
    LS_EQ_UINT(c[1].c, 0x2500);
    LS_EQ_UINT(c[2].c, 0x1f600);
    LS_EQ_UINT(c[3].c, 0);                               /* its right half */
    LS_EQ_UINT(c[4].c, '!');
    /* A broken sequence is one replacement character, then the text. */
    put(vt, "\r\n\xc3(");
    c = tmt_screen(vt)->lines[1]->chars;
    LS_EQ_UINT(c[0].c, TMT_INVALID_CHAR);
    LS_EQ_UINT(c[1].c, '(');
    /* DEC special graphics, as ncurses draws boxes under TERM=xterm. */
    put(vt, "\r\n\x1b(0lqk\x1b(Bq");
    c = tmt_screen(vt)->lines[2]->chars;
    LS_EQ_UINT(c[0].c, 0x250c);
    LS_EQ_UINT(c[1].c, 0x2500);
    LS_EQ_UINT(c[2].c, 0x2510);
    LS_EQ_UINT(c[3].c, 'q');
    tmt_close(vt);
}

LS_CASE(term_scroll_region_and_alt_screen)
{
    TMT *vt = tmt_open(5, 10, cb, NULL);
    put(vt, "a\r\nb\r\nc\r\nd\r\ne");
    /* Region rows 2..4: a linefeed at its bottom scrolls only inside it. */
    put(vt, "\x1b[2;4r\x1b[4;1H\n");
    LS_EQ_STR(row(vt, 0), "a");
    LS_EQ_STR(row(vt, 1), "c");
    LS_EQ_STR(row(vt, 2), "d");
    LS_EQ_STR(row(vt, 3), "");
    LS_EQ_STR(row(vt, 4), "e");
    /* Reverse index at the region's top scrolls it down. */
    put(vt, "\x1b[2;1H\x1bM");
    LS_EQ_STR(row(vt, 1), "");
    LS_EQ_STR(row(vt, 2), "c");
    put(vt, "\x1b[r");
    /* The alternate screen comes back to what was there. */
    put(vt, "\x1b[?1049h\x1b[2J\x1b[Hvim");
    LS_EQ_STR(row(vt, 0), "vim");
    put(vt, "\x1b[?1049l");
    LS_EQ_STR(row(vt, 0), "a");
    LS_EQ_STR(row(vt, 4), "e");
    /* Application cursor keys. */
    LS_CHECK(!tmt_app_cursor_keys(vt));
    put(vt, "\x1b[?1h");
    LS_CHECK(tmt_app_cursor_keys(vt));
    put(vt, "\x1b[?25l");
    LS_CHECK(!tmt_cursor_visible(vt));
    /* Resizing keeps the top-left and stays usable. */
    LS_CHECK(tmt_resize(vt, 3, 4));
    LS_EQ_STR(row(vt, 0), "a");
    put(vt, "\x1b[3;4Hz");
    LS_EQ_STR(row(vt, 2), "c  z");
    LS_CHECK(tmt_resize(vt, 8, 30));
    put(vt, "\x1b[8;30Hy");
    LS_EQ_INT(tmt_screen(vt)->lines[7]->chars[29].c, 'y');
    tmt_close(vt);
}

/* Whatever arrives from the network, the grid stays in bounds: random bytes
   biased toward escape sequences, with resizes in between (run under ASan). */
LS_CASE(term_random_input)
{
  for (uint32_t seed = 1; seed <= 6; ++seed) {
    TMT *vt = tmt_open(6, 12, cb, NULL);
    uint32_t x = seed * 2654435761u;
    static const char pick[] = "\x1b[];?0123456789HJKLMPX@mrhlABCDdfGsuS\r\n\b\t\x07 a\xc3\xa9\xe2\x94\x80";
    char buf[64];
    for (int round = 0; round < 4000; ++round) {
        for (size_t i = 0; i < sizeof(buf); ++i) {
            x = x * 1103515245u + 12345u;
            buf[i] = (x >> 24) & 1 ? pick[(x >> 8) % (sizeof(pick) - 1)] : (char)(x >> 16);
        }
        tmt_write(vt, buf, sizeof(buf));
        if (round % 500 == 499) LS_CHECK(tmt_resize(vt, 2 + round % 7, 2 + round % 13));
        const TMTPOINT *c = tmt_cursor(vt);
        LS_CHECK(c->r < tmt_screen(vt)->nline && c->c < tmt_screen(vt)->ncol);
    }
    tmt_close(vt);
  }
}
