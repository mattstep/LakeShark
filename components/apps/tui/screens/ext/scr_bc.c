/* BC: the bc calculator on the board's own screen and keyboard.

   The engine is components/lsbc (docs/BC.md); this is the scrollback of
   what was typed and what came back, the line being typed, and a touch
   keypad for when no keyboard is attached. The engine's context lives as
   long as the firmware does, so scale, variables and `last` are still there
   when the app is opened again. */

#include "sdkconfig.h"
#if defined(CONFIG_LS_BC) && CONFIG_LS_BC

#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_keyboard.h"
#include "../../ls_app.h"
#include "../../ls_term.h"
#include "esp_attr.h"

#include "bc.h"

#include <stdio.h>
#include <string.h>

#define A(fg, bg)  TUI_ATTR(fg, bg)
#define SCROLL     40                      /* lines kept */
#define LINE_W     (LS_TERM_INPUT_MAX + 8) /* an input line and its prompt */
#define INPUT_MAX  LS_TERM_INPUT_MAX
#define PROMPT     "> "

typedef enum { K_INPUT, K_RESULT, K_ERROR, K_INFO } kind_t;

EXT_RAM_BSS_ATTR static bc_ctx_t          s_bc;
EXT_RAM_BSS_ATTR static char              s_lines[SCROLL][LINE_W];
EXT_RAM_BSS_ATTR static uint8_t           s_kind[SCROLL];
EXT_RAM_BSS_ATTR static ls_term_history_t s_hist;
EXT_RAM_BSS_ATTR static char              s_in[INPUT_MAX + 1];
EXT_RAM_BSS_ATTR static char              s_draft[INPUT_MAX + 1];
EXT_RAM_BSS_ATTR static char              s_res[1024];
static bool     s_ready;
static int      s_head, s_count;      /* ring: s_head is the next slot */
static int      s_len, s_cur;         /* input length and cursor */
static int      s_hist_at;            /* 0: the line being typed; n: the n-th newest */
static int      s_back;               /* rows scrolled back from the newest */
static int      s_page = 8;
static bool     s_pad_flip;           /* the keypad shown against the keyboard rule */
static int      s_bar_n;
static tui_rect s_out_hit, s_in_hit, s_foot_hit;

/* --------------------------------------------------------- scrollback */

static void add_line(const char *text, int n, kind_t kind)
{
    if (n > LINE_W - 1) n = LINE_W - 1;
    memcpy(s_lines[s_head], text, (size_t)n);
    s_lines[s_head][n] = '\0';
    s_kind[s_head] = (uint8_t)kind;
    s_head = (s_head + 1) % SCROLL;
    if (s_count < SCROLL) s_count++;
}

/* The n-th newest stored line, n = 0 the newest. */
static int line_slot(int n) { return (s_head - 1 - n + 2 * SCROLL) % SCROLL; }

/* One output block from the engine, a line at a time. On an error the last
   three lines are the echo, the caret and the message. */
static void add_output(const char *out, bool error)
{
    int total = 0;
    for (const char *p = out; *p; p++) total += *p == '\n';
    int i = 0;
    for (const char *p = out; *p;) {
        const char *e = strchr(p, '\n');
        const int n = e ? (int)(e - p) : (int)strlen(p);
        add_line(p, n, error && i >= total - 3 ? K_ERROR : K_RESULT);
        i++;
        p += n + (e ? 1 : 0);
    }
}

/* ------------------------------------------------------------- the line */

static void set_input(const char *text)
{
    snprintf(s_in, sizeof(s_in), "%s", text ? text : "");
    s_len = (int)strlen(s_in);
    s_cur = s_len;
}

static void insert(const char *text)
{
    const int k = (int)strlen(text);
    if (s_len + k > INPUT_MAX) return;
    memmove(s_in + s_cur + k, s_in + s_cur, (size_t)(s_len - s_cur + 1));
    memcpy(s_in + s_cur, text, (size_t)k);
    s_len += k;
    s_cur += k;
    s_hist_at = 0;
}

static void erase(void)
{
    if (s_cur == 0) return;
    memmove(s_in + s_cur - 1, s_in + s_cur, (size_t)(s_len - s_cur + 1));
    s_len--;
    s_cur--;
}

static void evaluate(void)
{
    char line[INPUT_MAX + 1];             /* s_in is cleared below */
    snprintf(line, sizeof(line), "%s", s_in);
    char echo[LINE_W];
    snprintf(echo, sizeof(echo), PROMPT "%s", line);
    add_line(echo, (int)strlen(echo), K_INPUT);
    set_input("");
    s_hist_at = 0;
    s_back = 0;
    if (!line[0]) return;
    ls_term_history_add(&s_hist, line);
    const bc_status_t st = bc_run(&s_bc, line, s_res, sizeof(s_res));
    add_output(s_res, st == BC_ERROR);
    if (st == BC_QUIT) {
        /* An app has nowhere to quit to: say how to leave instead. */
        static const char note[] = "(quit: ESC or BACK leaves; the variables stay)";
        add_line(note, (int)sizeof(note) - 1, K_INFO);
    }
}

static void history(int dir)
{
    int at = s_hist_at + dir;
    if (at < 0) at = 0;
    if (at == s_hist_at) return;
    if (at > 0 && !ls_term_history_get(&s_hist, at)) return;   /* past the oldest */
    if (s_hist_at == 0 && at > 0) snprintf(s_draft, sizeof(s_draft), "%s", s_in);
    s_hist_at = at;
    set_input(at ? ls_term_history_get(&s_hist, at) : s_draft);
}

static void keys_done(const char *text)
{
    set_input(text);
    evaluate();
}

static void open_keys(void)
{
    ls_keyboard_open_plain("BC", s_in, INPUT_MAX, keys_done);
}

/* ----------------------------------------------------------- the keypad */

/* What each key types, or NULL for an action. Labels are what the glass
   shows; the order is a calculator's, read row by row. */
static const struct { const char *label, *value, *text; } PAD[] = {
    { "7", NULL, "7" }, { "8", NULL, "8" }, { "9", NULL, "9" }, { "/", NULL, "/" }, { "DEL", NULL, NULL },
    { "4", NULL, "4" }, { "5", NULL, "5" }, { "6", NULL, "6" }, { "*", NULL, "*" }, { "(", NULL, "(" },
    { "1", NULL, "1" }, { "2", NULL, "2" }, { "3", NULL, "3" }, { "-", NULL, "-" }, { ")", NULL, ")" },
    { "0", NULL, "0" }, { ".", NULL, "." }, { "^", NULL, "^" }, { "+", NULL, "+" }, { "=", "enter", NULL },
    { "BACK", "home", NULL }, { "ABC", "keyboard", NULL }, { "CLR", "line", NULL }, { "LAST", "result", "last" },
};
#define PAD_N  ((int)(sizeof(PAD) / sizeof(PAD[0])))
#define PAD_ROWS 5

static void pad(int i)
{
    if (i < 0 || i >= PAD_N) return;
    if (PAD[i].text) { insert(PAD[i].text); return; }
    if (!strcmp(PAD[i].label, "DEL"))       erase();
    else if (!strcmp(PAD[i].label, "="))    evaluate();
    else if (!strcmp(PAD[i].label, "BACK")) ls_tui_screen_show(0);
    else if (!strcmp(PAD[i].label, "ABC"))  open_keys();
    else if (!strcmp(PAD[i].label, "CLR"))  { set_input(""); s_hist_at = 0; }
}

static bool pad_shown(void) { return ls_tui_keyboard_mode() == s_pad_flip; }

/* ----------------------------------------------------------- the screen */

static void enter(void)
{
    if (!s_ready) {
        s_ready = true;
        bc_init(&s_bc);
        static const char hello[] =
            "bc: + - * / % ^, sqrt() s() c() a() l() e(), x=2, scale=2, obase=16. ENTER evaluates.";
        add_line(hello, (int)sizeof(hello) - 1, K_INFO);
    }
}

static uint8_t kind_attr(int kind)
{
    switch (kind) {
    case K_INPUT:  return A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    case K_ERROR:  return A(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    case K_INFO:   return LS_ATTR_DIM;
    default:       return A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    }
}

static int rows_of(const char *s, int w)
{
    const int n = (int)strlen(s);
    return n <= w ? 1 : (n + w - 1) / w;
}

/* Newest at the bottom, wrapped at the width, `s_back` rows scrolled up. */
static void draw_scroll(tui_surface *sf, tui_rect out)
{
    const int w = out.w < LINE_W ? out.w : LINE_W;
    int total = 0;
    for (int n = 0; n < s_count; n++) total += rows_of(s_lines[line_slot(n)], w);
    const int most = total > out.h ? total - out.h : 0;
    if (s_back > most) s_back = most;
    int y = out.y + out.h - 1 + s_back;
    char cut[LINE_W];
    for (int n = 0; n < s_count && y >= out.y; n++) {
        const int slot = line_slot(n);
        const char *s = s_lines[slot];
        const int rows = rows_of(s, w);
        for (int r = rows - 1; r >= 0 && y >= out.y; r--, y--) {
            if (y >= out.y + out.h) continue;
            snprintf(cut, sizeof(cut), "%.*s", w, s + r * w);
            tui_put_str(sf, out, out.x, y, cut, kind_attr(s_kind[slot]));
        }
    }
}

static void draw_input(tui_surface *sf, tui_rect in, int y)
{
    s_in_hit = tui_rect_make(in.x, y, in.w, 1);
    const int pw = (int)sizeof(PROMPT) - 1;
    tui_put_str(sf, in, in.x, y, PROMPT, A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    const int room = in.w - pw - 1;
    if (room < 1) return;
    /* Scroll the text sideways so the cursor stays on the glass. */
    const int first = s_cur > room ? s_cur - room : 0;
    char shown[INPUT_MAX + 1];
    snprintf(shown, sizeof(shown), "%.*s", room, s_in + first);
    const int x0 = in.x + pw;
    tui_put_str(sf, in, x0, y, shown, A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    const int cx = x0 + s_cur - first;
    const char under = s_cur < s_len ? s_in[s_cur] : ' ';
    tui_put_char(sf, in, cx, y, under, A(TUI_BLACK, TUI_GREEN | TUI_BRIGHT));
}

static void draw(tui_surface *sf, tui_rect a)
{
    s_out_hit = s_in_hit = s_foot_hit = tui_rect_make(0, 0, 0, 0);
    s_bar_n = 0;
    if (a.w < 20 || a.h < 8) { ls_panel_notice(sf, a, "BC", "Enlarge the pane", NULL); return; }

    if (pad_shown()) {
        /* Five rows of keys, each as tall as the pane can spare up to the
           helper's own cap, never more than half the pane. */
        int h = a.h / 2;
        if (h > PAD_ROWS * 5) h = PAD_ROWS * 5;
        h -= h % PAD_ROWS;
        if (h >= PAD_ROWS && a.h - h >= 6) {
            ls_btn_t btn[PAD_N];
            for (int i = 0; i < PAD_N; i++)
                btn[i] = (ls_btn_t){ PAD[i].label, PAD[i].value, 0, false, false, false };
            ls_btn_bar_rows_slot(sf, tui_rect_make(a.x, a.y + a.h - h, a.w, h), btn, PAD_N, -1,
                                 LS_BTN_SLOT_SCREEN, PAD_ROWS);
            s_bar_n = PAD_N;
            a.h -= h;
        }
    }

    tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - 1);
    ls_panel_box(sf, body, "BC", TUI_YELLOW);
    tui_rect in = tui_rect_make(body.x + 2, body.y + 1, body.w - 4, body.h - 2);
    if (in.w < 8 || in.h < 2) return;
    tui_rect out = tui_rect_make(in.x, in.y, in.w, in.h - 1);
    s_page = out.h > 2 ? out.h - 2 : 1;
    draw_scroll(sf, out);
    s_out_hit = out;
    if (s_back) {
        char mark[24];
        snprintf(mark, sizeof(mark), " back %d ", s_back);
        tui_put_str(sf, body, body.x + body.w - 2 - (int)strlen(mark), body.y, mark, A(TUI_BLACK, TUI_YELLOW));
    }
    draw_input(sf, in, in.y + in.h - 1);

    char foot[96];
    snprintf(foot, sizeof(foot), "scale %d  ibase %d  obase %d  vars %d/%d%s", s_bc.scale, s_bc.ibase,
             s_bc.obase, s_bc.nvars, BC_VARS,
             pad_shown() ? (ls_tui_keyboard_mode() ? "  tap: hide keys" : "") : "  tap: keypad");
    const int fy = a.y + a.h - 1;
    s_foot_hit = tui_rect_make(a.x, fy, a.w, 1);
    ls_safe_line(sf, a, fy, foot, LS_ATTR_DIM);
}

static bool key(ls_tk_t k, char ch)
{
    switch (k) {
    case LS_TK_CHAR:
        if ((unsigned char)ch >= 0x20 && (unsigned char)ch < 0x7F) {
            const char t[2] = { ch, '\0' };
            insert(t);
        }
        return true;
    case LS_TK_BACKSPACE: erase(); return true;
    case LS_TK_ENTER:     evaluate(); return true;
    case LS_TK_LEFT:      if (s_cur > 0) s_cur--; return true;
    case LS_TK_RIGHT:     if (s_cur < s_len) s_cur++; return true;
    case LS_TK_UP:        history(1); return true;
    case LS_TK_DOWN:      history(-1); return true;
    case LS_TK_ESC:
        if (s_len) { set_input(""); s_hist_at = 0; return true; }
        return false;                                   /* HOME */
    default:
        return false;
    }
}

static bool touch(int x, int y)
{
    const int i = ls_btn_hit(x, y);
    if (i >= 0 && i < s_bar_n) { pad(i); return true; }
    if (s_in_hit.w > 0 && tui_rect_contains(s_in_hit, x, y)) { open_keys(); return true; }
    if (s_foot_hit.w > 0 && tui_rect_contains(s_foot_hit, x, y)) { s_pad_flip = !s_pad_flip; return true; }
    if (s_out_hit.w > 0 && tui_rect_contains(s_out_hit, x, y)) {
        s_back += y < s_out_hit.y + s_out_hit.h / 2 ? s_page : -s_page;
        if (s_back < 0) s_back = 0;                     /* draw caps the other end */
        return true;
    }
    return true;           /* a tap anywhere else is not ENTER */
}

const ls_tui_screen_t ls_scr_bc = {
    .name  = "BC",
    .hint  = "ENTER evaluate  UP/DOWN history  LEFT/RIGHT cursor  ESC clear, then home",
    .enter = enter,
    .draw  = draw,
    .key   = key,
    .touch = touch,
};

const ls_app_doc_t ls_doc_bc = {
    .purpose = "A calculator in bc's syntax: arithmetic, powers, variables, "
               "square roots and the sine/cosine/arctan/log/exp functions, "
               "with the answers to the number of places `scale` asks for.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

#endif
