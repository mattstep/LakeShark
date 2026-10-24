/* SSH: an interactive shell on a remote host, with the board's own key.

   HOSTS lists the pinned servers (ls_ssh_store.h); NEW asks for
   user@host[:port]; KEY shows the board's public key for authorized_keys.
   Connecting shows the server's fingerprint the first time and pins it on
   TRUST; a changed key is refused. Then the whole pane is the remote
   terminal, drawn from libtmt.

   Who does what: the session worker (components/lsssh) moves bytes between
   the socket and two rings and never touches flash. This screen, on the UI
   task, drains the output ring into the emulator from draw() - so the
   emulator needs no lock - and does every NVS read and write. */

#include "sdkconfig.h"
#if defined(CONFIG_LS_SSH) && CONFIG_LS_SSH

#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_keyboard.h"
#include "../../ls_app.h"
#include "../../ls_ssh_keys.h"
#include "esp_attr.h"

#include "ls_ssh_session.h"
#include "ls_ssh_store.h"
#include "tmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A(fg, bg)    TUI_ATTR(fg, bg)
#define MAX_HOSTS    12
#define RX_BUDGET    (16 * 1024)    /* bytes fed to the emulator per frame */
#define TERM_NAME    "xterm"

typedef enum { V_HOSTS, V_KEY, V_CONNECT, V_FAIL, V_TERM } view_t;

/* Screen state. One SSH screen exists, so these are plain statics; the large
   ones are in PSRAM. */
static view_t s_view;
EXT_RAM_BSS_ATTR static ls_ssh_host_t s_hosts[MAX_HOSTS];
static int s_nhosts, s_sel;
static bool s_forget_armed, s_keygen_armed;
static int s_guess_cols = 80, s_guess_rows = 24;  /* the pty to ask for, from the last pane */
EXT_RAM_BSS_ATTR static char s_pub[160];
static bool s_have_id;
EXT_RAM_BSS_ATTR static char s_note[112];

static ls_ssh_session_t *s_sess;
EXT_RAM_BSS_ATTR static ls_ssh_host_t s_target;
static bool s_opened, s_closed;        /* reached OPEN; and ended since */
EXT_RAM_BSS_ATTR static ls_ssh_info_t s_info;  /* last copy, kept after the session is gone */

static TMT *s_vt;
static int s_cols, s_rows;
static ls_ssh_mods_t s_mods;
static bool s_bar_on;
EXT_RAM_BSS_ATTR static char s_rx[2048];

static tui_rect s_list_hit, s_status_hit;
static int s_list_top, s_list_rows;
static int s_bar_n;

static void note(const char *fmt, const char *arg)
{
    snprintf(s_note, sizeof(s_note), fmt, arg ? arg : "");
}

/* --------------------------------------------------------------- hosts */

static void load_hosts(void)
{
    s_nhosts = ls_ssh_store_hosts(s_hosts, MAX_HOSTS);
    if (s_sel > s_nhosts + 1) s_sel = 0;
    s_have_id = ls_ssh_store_has_identity();
}

static void show_hosts(void)
{
    s_view = V_HOSTS;
    s_forget_armed = false;
    ls_tui_screen_set_bare(false);
    load_hosts();
}

static void tmt_cb(tmt_msg_t m, TMT *vt, const void *r, void *p)
{
    (void)vt; (void)p;
    /* Cursor reports and device attributes: the remote asked, answer it. */
    if (m == TMT_MSG_ANSWER && s_sess) ls_ssh_session_write(s_sess, r, strlen(r));
}

static void connect_to(const char *host, unsigned port, const char *user)
{
    if (s_sess) return;
    char *id = ls_ssh_store_identity();
    if (!id) {
        note("No key yet. Make one here, then add it to the server.%s", NULL);
        s_view = V_KEY;
        return;
    }
    snprintf(s_target.host, sizeof(s_target.host), "%s", host);
    snprintf(s_target.user, sizeof(s_target.user), "%s", user);
    s_target.port = port ? port : 22;
    char *rec = ls_ssh_store_host_record(s_target.host, s_target.port);
    const ls_ssh_params_t p = {
        .host = s_target.host, .port = s_target.port, .user = s_target.user,
        .identity = id, .pinned = ls_ssh_store_record_key(rec),
        .mode = LS_SSH_SHELL, .term = TERM_NAME,
        .cols = s_cols > 0 ? s_cols : s_guess_cols, .rows = s_rows > 0 ? s_rows : s_guess_rows,
    };
    char why[96];
    s_sess = ls_ssh_session_open(&p, why, sizeof(why));
    ls_ssh_free_secret(id);
    free(rec);
    memset(&s_info, 0, sizeof(s_info));
    s_opened = s_closed = false;
    s_mods = (ls_ssh_mods_t){0};
    s_note[0] = 0;
    if (!s_sess) {
        snprintf(s_info.message, sizeof(s_info.message), "%s", why);
        s_info.end = LS_SSH_END_NOMEM;
        s_view = V_FAIL;
        return;
    }
    if (s_vt) tmt_reset(s_vt);
    s_view = V_CONNECT;
}

/* "user@host[:port]" from the keyboard. */
static void new_done(const char *text)
{
    while (*text == ' ') ++text;
    const char *at = strchr(text, '@');
    if (!at || at == text || !at[1]) { note("Give it as user@host or user@host:port%s", NULL); return; }
    char user[32], host[64];
    unsigned port = 22;
    snprintf(user, sizeof(user), "%.*s", (int)(at - text), text);
    ls_ssh_split_hostport(at + 1, host, sizeof(host), &port);
    char *sp = strchr(host, ' ');
    if (sp) *sp = 0;
    connect_to(host, port, user);
}

static void open_new(const char *initial)
{
    ls_keyboard_open_plain("USER@HOST[:PORT]", initial ? initial : "", 95, new_done);
}

/* A pinned host with no remembered user (pinned from the console): ask. */
static void user_done(const char *text)
{
    while (*text == ' ') ++text;
    if (!*text || s_sel >= s_nhosts) return;
    connect_to(s_hosts[s_sel].host, s_hosts[s_sel].port, text);
}

static void open_selected(void)
{
    if (s_sel < s_nhosts) {
        const ls_ssh_host_t *h = &s_hosts[s_sel];
        if (h->user[0]) connect_to(h->host, h->port, h->user);
        else ls_keyboard_open_plain("USER", "", 31, user_done);
    } else if (s_sel == s_nhosts) {
        open_new(NULL);
    } else {
        s_view = V_KEY;
        s_pub[0] = 0;
    }
}

static void forget_selected(void)
{
    if (s_sel >= s_nhosts) return;
    if (!s_forget_armed) {
        s_forget_armed = true;
        note("FORGET again to drop %s", s_hosts[s_sel].host);
        return;
    }
    esp_err_t e = ls_ssh_store_forget(s_hosts[s_sel].host, s_hosts[s_sel].port);
    note(e == ESP_OK ? "Forgotten%s" : "Could not forget it%s", NULL);
    s_forget_armed = false;
    load_hosts();
}

/* ----------------------------------------------------------------- key */

static void keygen(void)
{
    char *priv = NULL, err[96];
    if (s_have_id && !s_keygen_armed) {
        s_keygen_armed = true;
        note("KEYGEN again to replace the key; servers will need the new one%s", NULL);
        return;
    }
    s_keygen_armed = false;
    /* Blocks the UI for the few hundred ms an Ed25519 key takes; it is
       pressed once in the life of the board. */
    if (!ls_ssh_keygen(&priv, s_pub, sizeof(s_pub), err, sizeof(err))) { note("%s", err); return; }
    esp_err_t e = ls_ssh_store_set_identity(priv);
    ls_ssh_free_secret(priv);
    if (e != ESP_OK) { s_pub[0] = 0; note("Could not store the key: %s", esp_err_to_name(e)); return; }
    s_have_id = true;
    note("New key made. Add it to ~/.ssh/authorized_keys on the server.%s", NULL);
}

static void load_pub(void)
{
    if (s_pub[0] || !s_have_id) return;
    char *id = ls_ssh_store_identity();
    if (!id) { s_have_id = false; return; }
    /* Once per visit: a failure leaves a marker, not an empty line to retry
       (and a worker to start) every frame. */
    if (!ls_ssh_pubkey_line(id, s_pub, sizeof(s_pub))) {
        snprintf(s_pub, sizeof(s_pub), "(unreadable)");
        note("The stored key is unreadable; KEYGEN makes a new one%s", NULL);
    }
    ls_ssh_free_secret(id);
}

/* ------------------------------------------------------------- session */

static void end_session(void)
{
    if (!s_sess) return;
    ls_ssh_session_info(s_sess, &s_info);
    ls_ssh_session_free(s_sess);
    s_sess = NULL;
}

static void trust(bool yes)
{
    if (!s_sess || ls_ssh_session_state(s_sess) != LS_SSH_HOSTKEY_UNKNOWN) return;
    if (yes) {
        const char *key = ls_ssh_session_server_key(s_sess);
        esp_err_t e = key ? ls_ssh_store_pin(s_target.host, s_target.port, key) : ESP_FAIL;
        if (e != ESP_OK) note("Trusted for now; pinning failed (%s)", esp_err_to_name(e));
    }
    ls_ssh_session_trust(s_sess, yes);
}

static void feed(void)
{
    if (!s_sess) return;
    size_t got = 0, n;
    while (got < RX_BUDGET && (n = ls_ssh_session_read(s_sess, s_rx, sizeof(s_rx))) > 0) {
        if (s_vt) tmt_write(s_vt, s_rx, n);
        got += n;
    }
}

/* Where the session is, from the UI task, once a frame. */
static void poll_session(void)
{
    if (!s_sess) return;
    const ls_ssh_state_t st = ls_ssh_session_state(s_sess);
    if (st == LS_SSH_OPEN && !s_opened) {
        s_opened = true;
        s_view = V_TERM;
        /* NVS from the UI task, as the worker must not. */
        ls_ssh_store_set_user(s_target.host, s_target.port, s_target.user);
    }
    feed();
    if (st == LS_SSH_CLOSED) {
        feed();                                  /* the last words */
        end_session();
        s_closed = true;
        if (!s_opened) {
            if (s_info.end == LS_SSH_END_LOCAL) show_hosts();   /* CANCEL */
            else s_view = V_FAIL;
        }
    } else {
        ls_ssh_session_info(s_sess, &s_info);
    }
}

static void send(const char *b, int n)
{
    if (s_sess && n > 0) ls_ssh_session_write(s_sess, b, (size_t)n);
}

static void keys_done(const char *text)
{
    send(text, (int)strlen(text));
    send("\r", 1);
}

static void disconnect(void)
{
    if (s_sess) ls_ssh_session_close(s_sess);
    else show_hosts();
}

/* ----------------------------------------------------------- rendering */

/* One cell for a codepoint: ASCII as itself, line drawing as ASCII, other
   text through the TUI's own fold (Latin accents, Cyrillic), else '?'. */
static char glyph(uint32_t cp)
{
    if (cp >= 0x20 && cp < 0x7f) return (char)cp;
    if (cp < 0x20 || cp == 0x7f) return ' ';
    if (cp >= 0x2500 && cp <= 0x257f) {
        switch (cp) {
        case 0x2500: case 0x2501: case 0x2504: case 0x2505: case 0x2508: case 0x2509:
        case 0x254c: case 0x254d: case 0x2550: case 0x2574: case 0x2576: case 0x2578:
        case 0x257a: case 0x257c: case 0x257e:
            return '-';
        case 0x2502: case 0x2503: case 0x2506: case 0x2507: case 0x250a: case 0x250b:
        case 0x254e: case 0x254f: case 0x2551: case 0x2575: case 0x2577: case 0x2579:
        case 0x257b: case 0x257d: case 0x257f:
            return '|';
        case 0x2571: return '/';
        case 0x2572: return '\\';
        case 0x2573: return 'X';
        default:     return '+';
        }
    }
    switch (cp) {
    case 0x2588: return LS_TUI_BLOCK_FULL;
    case 0x2580: return LS_TUI_BLOCK_UPPER;
    case 0x2584: return LS_TUI_BLOCK_LOWER;
    case 0x258c: return LS_TUI_BLOCK_LEFT;
    case 0x2590: return LS_TUI_BLOCK_RIGHT;
    case 0x2591: return LS_TUI_SHADE_25;
    case 0x2592: return LS_TUI_SHADE_50;
    case 0x2593: return LS_TUI_SHADE_75;
    case 0x23ba: case 0x23bb: case 0x23bc: case 0x23bd: return '-';
    case 0x25c6: case 0x2022: case 0x25cf: return '*';
    case 0x00b7: return '.';
    case 0x00a0: return ' ';
    default: break;
    }
    return tui_cell_codepoint_byte(cp);
}

static uint8_t color(int c, uint8_t dflt)
{
    if (c >= TMT_COLOR_BLACK && c <= TMT_COLOR_WHITE) return (uint8_t)(c - TMT_COLOR_BLACK);
    if (c >= TMT_COLOR_BRIGHT_BLACK && c <= TMT_COLOR_BRIGHT_WHITE)
        return (uint8_t)((c - TMT_COLOR_BRIGHT_BLACK) | TUI_BRIGHT);
    return dflt;
}

static void draw_term(tui_surface *sf, tui_rect r)
{
    if (!s_vt) { ls_panel_notice(sf, r, "SSH", "No memory for the terminal", "Close a radio app and reconnect"); return; }
    const TMTSCREEN *scr = tmt_screen(s_vt);
    const TMTPOINT *cur = tmt_cursor(s_vt);
    const bool show_cur = tmt_cursor_visible(s_vt) && !s_closed;
    for (size_t y = 0; y < scr->nline && (int)y < r.h; ++y) {
        const TMTLINE *line = scr->lines[y];
        for (size_t x = 0; x < scr->ncol && (int)x < r.w; ++x) {
            const TMTCHAR *c = &line->chars[x];
            uint8_t fg = color(c->a.fg, TUI_WHITE), bg = color(c->a.bg, TUI_BLACK);
            if (c->a.bold) fg |= TUI_BRIGHT;
            bool rev = c->a.reverse;
            if (show_cur && y == cur->r && x == cur->c) rev = !rev;
            if (rev) { uint8_t t = fg; fg = bg; bg = t; }
            if (c->a.invisible) fg = bg;
            tui_put_char(sf, r, r.x + (int)x, r.y + (int)y, glyph(c->c), A(fg, bg));
        }
    }
}

static void draw_status(tui_surface *sf, tui_rect a, int y)
{
    s_status_hit = tui_rect_make(a.x, y, a.w, 1);
    tui_fill(sf, s_status_hit, ' ', A(TUI_BLACK, TUI_CYAN));
    char left[96];
    snprintf(left, sizeof(left), " %s@%s %dx%d ", s_target.user, s_target.host, s_cols, s_rows);
    tui_put_str(sf, s_status_hit, a.x, y, left, A(TUI_BLACK, TUI_CYAN));
    int x = a.x + (int)strlen(left);
    const struct { bool on; const char *t; } mods[] = {
        { s_mods.ctrl, "CTRL" }, { s_mods.alt, "ALT" }, { s_mods.fn, "FN" } };
    for (unsigned i = 0; i < 3; ++i) if (mods[i].on) {
        tui_put_str(sf, s_status_hit, x, y, mods[i].t, A(TUI_WHITE | TUI_BRIGHT, TUI_RED));
        x += (int)strlen(mods[i].t) + 1;
    }
    char right[140];
    if (s_closed) snprintf(right, sizeof(right), "%s - ENTER: hosts ", s_info.message);
    else if (s_note[0]) snprintf(right, sizeof(right), "%s ", s_note);
    else snprintf(right, sizeof(right), "%s", ls_tui_keyboard_mode() ? "FN Q quit  tap: buttons " : "tap: buttons ");
    int rx = a.x + a.w - (int)strlen(right);
    if (rx < x + 1) rx = x + 1;
    tui_put_str(sf, s_status_hit, rx, y, right,
                s_closed ? A(TUI_WHITE | TUI_BRIGHT, TUI_RED) : A(TUI_BLACK, TUI_CYAN));
}

static const ls_btn_t *term_buttons(int *n)
{
    static ls_btn_t b[8];
    b[0] = (ls_btn_t){ "KEYS", "type", 0, false, s_closed, false };
    b[1] = (ls_btn_t){ "CTRL", "next", 0, s_mods.ctrl, s_closed, false };
    b[2] = (ls_btn_t){ "ESC", NULL, 0, false, s_closed, false };
    b[3] = (ls_btn_t){ "TAB", NULL, 0, false, s_closed, false };
    b[4] = (ls_btn_t){ "UP", NULL, 0, false, s_closed, false };
    b[5] = (ls_btn_t){ "DOWN", NULL, 0, false, s_closed, false };
    b[6] = (ls_btn_t){ "ENTER", NULL, 0, false, s_closed, false };
    b[7] = (ls_btn_t){ s_closed ? "HOSTS" : "DISC", s_closed ? "back" : "close", 0, false, false, false };
    *n = 8;
    return b;
}

static void term_view(tui_surface *sf, tui_rect a)
{
    int bar_h = 0, nb = 0;
    const ls_btn_t *btn = term_buttons(&nb);
    s_bar_n = 0;
    if (s_bar_on) {
        bar_h = a.w > a.h * 2 ? 3 : ls_btn_raised_height(a, nb);
        if (bar_h > a.h / 2) bar_h = 3;
    }
    tui_rect tr = tui_rect_make(a.x, a.y, a.w, a.h - 1 - bar_h);
    if (tr.w < 10 || tr.h < 3) { ls_panel_notice(sf, a, "SSH", "Enlarge the pane", NULL); return; }

    /* The pane is the pty: follow it when it turns or the bar comes and goes. */
    if (s_vt && (tr.w != s_cols || tr.h != s_rows)) {
        if (tmt_resize(s_vt, (size_t)tr.h, (size_t)tr.w)) {
            s_cols = tr.w;
            s_rows = tr.h;
            if (s_sess) ls_ssh_session_resize(s_sess, s_cols, s_rows);
        } else {
            tmt_close(s_vt);
            s_vt = NULL;
        }
    }
    tui_fill(sf, tr, ' ', A(TUI_WHITE, TUI_BLACK));
    draw_term(sf, tr);
    draw_status(sf, a, tr.y + tr.h);
    if (bar_h) {
        ls_btn_bar_raised(sf, tui_rect_make(a.x, tr.y + tr.h + 1, a.w, bar_h), btn, nb, -1);
        s_bar_n = nb;
    }
}

static void term_button(int i)
{
    char out[LS_SSH_KEY_MAX];
    int n = 0;
    static const ls_tk_t K[] = { LS_TK_NONE, LS_TK_CTRL, LS_TK_ESC, LS_TK_TAB,
                                 LS_TK_UP, LS_TK_DOWN, LS_TK_ENTER };
    if (i == 7) { if (s_closed) show_hosts(); else disconnect(); return; }
    if (s_closed) return;
    if (i == 0) { ls_keyboard_open_plain("SEND (ENTER FOLLOWS)", "", 200, keys_done); return; }
    if (i > 0 && i < 7 &&
        ls_ssh_key_encode(&s_mods, K[i], 0, s_vt && tmt_app_cursor_keys(s_vt), out, &n) == LS_SSH_KEY_SEND)
        send(out, n);
}

static void list_view(tui_surface *sf, tui_rect a)
{
    const ls_btn_t btn[] = {
        { "OPEN", "connect", 0, false, false, false },
        { "NEW", "user@host", 'n', false, false, false },
        { "KEY", "board key", 'k', false, false, false },
        { "FORGET", "host", 'f', s_forget_armed, s_sel >= s_nhosts, false },
    };
    int bar_h = a.w > a.h * 2 ? 3 : ls_btn_raised_height(a, 4);
    if (bar_h > a.h / 2) bar_h = 3;
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y + a.h - bar_h, a.w, bar_h), btn, 4, -1);
    s_bar_n = 4;

    tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - bar_h - 1);
    ls_panel_box(sf, body, "SSH HOSTS", TUI_GREEN);
    tui_rect in = tui_rect_make(body.x + 2, body.y + 1, body.w - 4, body.h - 2);
    if (in.w < 8 || in.h < 2) return;
    const int total = s_nhosts + 2;
    s_list_rows = in.h;
    if (s_sel < s_list_top) s_list_top = s_sel;
    if (s_sel >= s_list_top + in.h) s_list_top = s_sel - in.h + 1;
    s_list_hit = in;
    for (int row = 0; row < in.h && s_list_top + row < total; ++row) {
        const int i = s_list_top + row;
        char line[112];
        if (i < s_nhosts) {
            const ls_ssh_host_t *h = &s_hosts[i];
            if (h->port == 22) snprintf(line, sizeof(line), "%s%s%s", h->user, h->user[0] ? "@" : "", h->host);
            else snprintf(line, sizeof(line), "%s%s%s:%u", h->user, h->user[0] ? "@" : "", h->host, h->port);
        } else if (i == s_nhosts) {
            snprintf(line, sizeof(line), "NEW...  connect to user@host[:port]");
        } else {
            snprintf(line, sizeof(line), "KEY     %s", s_have_id ? "the board's public key" : "none yet: make one");
        }
        const bool sel = i == s_sel;
        if (sel) tui_fill(sf, tui_rect_make(in.x, in.y + row, in.w, 1), ' ', A(TUI_BLACK, TUI_GREEN));
        tui_put_str(sf, in, in.x, in.y + row, line,
                    sel ? A(TUI_BLACK, TUI_GREEN) : i < s_nhosts ? A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
    }
    if (!s_nhosts && in.h > 4)
        ls_safe_line(sf, in, in.y + 3, "No pinned hosts yet: NEW connects and pins one", LS_ATTR_DIM);
    ls_safe_line(sf, a, a.y + a.h - bar_h - 1, s_note, A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
}

/* Text wrapped at w, for keys and fingerprints that will not fit a row. */
static int wrap_lines(tui_surface *sf, tui_rect r, int y, const char *text, uint8_t attr)
{
    const int len = (int)strlen(text);
    char cut[160];
    int w = r.w < (int)sizeof(cut) - 1 ? r.w : (int)sizeof(cut) - 1;
    if (w <= 0) return y;
    for (int off = 0; off < len && y < r.y + r.h; off += w, ++y) {
        snprintf(cut, sizeof(cut), "%.*s", w, text + off);
        tui_put_str(sf, r, r.x, y, cut, attr);
    }
    return y;
}

static void key_view(tui_surface *sf, tui_rect a)
{
    load_pub();
    const ls_btn_t btn[] = {
        { "BACK", "hosts", 0, false, false, false },
        { "KEYGEN", s_have_id ? "replace" : "make", 0, false, false, false },
    };
    int bar_h = a.w > a.h * 2 ? 3 : ls_btn_raised_height(a, 2);
    if (bar_h > a.h / 2) bar_h = 3;
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y + a.h - bar_h, a.w, bar_h), btn, 2, -1);
    s_bar_n = 2;
    tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - bar_h - 1);
    ls_panel_box(sf, body, "BOARD KEY", TUI_CYAN);
    tui_rect in = tui_rect_make(body.x + 2, body.y + 1, body.w - 4, body.h - 2);
    int y = in.y;
    if (!s_have_id) {
        y = wrap_lines(sf, in, y, "This board has no SSH key yet. KEYGEN makes an Ed25519 key that never leaves the board.", LS_ATTR_DIM);
    } else {
        y = wrap_lines(sf, in, y, "Add this line to ~/.ssh/authorized_keys on the server (also: 'ssh pubkey' on the console):", LS_ATTR_DIM);
        y = wrap_lines(sf, in, y + 1, s_pub[0] ? s_pub : "(reading...)", A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        if (y + 1 < in.y + in.h)
            wrap_lines(sf, in, y + 1, "KEYGEN replaces it: every server then needs the new line.", LS_ATTR_DIM);
    }
    ls_safe_line(sf, a, a.y + a.h - bar_h - 1, s_note, A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
}

static const char *state_words(ls_ssh_state_t st)
{
    switch (st) {
    case LS_SSH_CONNECTING:     return "Connecting...";
    case LS_SSH_AUTHENTICATING: return "Logging in with the board key...";
    case LS_SSH_OPEN:           return "Opening the shell...";
    default:                    return "";
    }
}

static void connect_view(tui_surface *sf, tui_rect a)
{
    const ls_ssh_state_t st = s_sess ? ls_ssh_session_state(s_sess) : LS_SSH_CLOSED;
    const bool ask = st == LS_SSH_HOSTKEY_UNKNOWN;
    const bool changed = s_view == V_FAIL && s_info.end == LS_SSH_END_HOSTKEY_CHANGED;
    const bool auth = s_view == V_FAIL && s_info.end == LS_SSH_END_AUTH;
    ls_btn_t btn[3];
    int nb = 0;
    if (ask) {
        btn[nb++] = (ls_btn_t){ "TRUST", "pin key", 't', false, false, false };
        btn[nb++] = (ls_btn_t){ "CANCEL", NULL, 'c', false, false, false };
    } else if (s_view == V_FAIL) {
        btn[nb++] = (ls_btn_t){ "BACK", "hosts", 0, false, false, false };
        btn[nb++] = (ls_btn_t){ "RETRY", NULL, 'r', false, changed, false };
        if (changed) btn[nb++] = (ls_btn_t){ "FORGET", "old key", 'f', false, false, false };
        if (auth) btn[nb++] = (ls_btn_t){ "KEY", "show", 'k', false, false, false };
    } else {
        btn[nb++] = (ls_btn_t){ "CANCEL", NULL, 'c', false, false, false };
    }
    int bar_h = a.w > a.h * 2 ? 3 : ls_btn_raised_height(a, nb);
    if (bar_h > a.h / 2) bar_h = 3;
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y + a.h - bar_h, a.w, bar_h), btn, nb, -1);
    s_bar_n = nb;

    tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - bar_h - 1);
    char title[96];
    snprintf(title, sizeof(title), "%s@%s:%u", s_target.user, s_target.host, s_target.port);
    ls_panel_box(sf, body, title, changed || s_view == V_FAIL ? TUI_RED : ask ? TUI_YELLOW : TUI_CYAN);
    tui_rect in = tui_rect_make(body.x + 2, body.y + 1, body.w - 4, body.h - 2);
    int y = in.y;
    if (ask) {
        y = wrap_lines(sf, in, y, "First connection to this host. Check the fingerprint against the server's own:", LS_ATTR_DIM);
        y = wrap_lines(sf, in, y + 1, s_info.key_type, A(TUI_WHITE, TUI_BLACK));
        y = wrap_lines(sf, in, y, s_info.fingerprint, A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        wrap_lines(sf, in, y + 1, "(ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub on the server.) TRUST pins it.", LS_ATTR_DIM);
    } else if (s_view == V_FAIL) {
        if (changed) {
            y = wrap_lines(sf, in, y, "THE HOST KEY HAS CHANGED. Refused.", A(TUI_RED | TUI_BRIGHT, TUI_BLACK));
            y = wrap_lines(sf, in, y + 1, "Either the server was reinstalled or someone is in the middle. FORGET drops the pinned key only if you know why it changed.", LS_ATTR_DIM);
            if (s_info.fingerprint[0]) {
                y = wrap_lines(sf, in, y + 1, "It now offers:", LS_ATTR_DIM);
                wrap_lines(sf, in, y, s_info.fingerprint, A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
            }
        } else if (s_info.end == LS_SSH_END_HOSTKEY_REFUSED) {
            wrap_lines(sf, in, y, "Not trusted; nothing was pinned. RETRY to see the fingerprint again.", LS_ATTR_DIM);
        } else {
            y = wrap_lines(sf, in, y, s_info.message, A(TUI_RED | TUI_BRIGHT, TUI_BLACK));
            if (auth) wrap_lines(sf, in, y + 1, "KEY shows the line for the server's authorized_keys.", LS_ATTR_DIM);
            else if (s_info.end == LS_SSH_END_CONNECT)
                wrap_lines(sf, in, y + 1, "Is Wi-Fi up (LINK) and the host reachable?", LS_ATTR_DIM);
        }
    } else {
        y = wrap_lines(sf, in, y, state_words(st), A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        if (s_info.fingerprint[0]) wrap_lines(sf, in, y + 1, s_info.fingerprint, LS_ATTR_DIM);
    }
    ls_safe_line(sf, a, a.y + a.h - bar_h - 1, s_note, A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
}

static void connect_button(int i)
{
    const ls_ssh_state_t st = s_sess ? ls_ssh_session_state(s_sess) : LS_SSH_CLOSED;
    if (st == LS_SSH_HOSTKEY_UNKNOWN) { trust(i == 0); return; }
    if (s_view != V_FAIL) { disconnect(); return; }
    const bool changed = s_info.end == LS_SSH_END_HOSTKEY_CHANGED;
    if (i == 0) show_hosts();
    else if (i == 1 && !changed) connect_to(s_target.host, s_target.port, s_target.user);
    else if (i == 2 && changed) {
        ls_ssh_store_forget(s_target.host, s_target.port);
        note("Old key forgotten. RETRY shows the new fingerprint.%s", NULL);
        s_info.end = LS_SSH_END_HOSTKEY_REFUSED;    /* RETRY is offered again */
    } else if (i == 2) { s_pub[0] = 0; s_view = V_KEY; }
}

/* ----------------------------------------------------------- the screen */

static void enter(void)
{
    s_mods = (ls_ssh_mods_t){0};
    s_note[0] = 0;
    s_bar_on = !ls_tui_keyboard_mode();
    s_cols = s_rows = 0;
    /* The emulator lives only while the screen does; its grid is in PSRAM. */
    s_vt = tmt_open(24, 80, tmt_cb, NULL);
    show_hosts();
}

static void leave(void)
{
    if (s_sess) { ls_ssh_session_free(s_sess); s_sess = NULL; }
    if (s_vt) { tmt_close(s_vt); s_vt = NULL; }
    s_view = V_HOSTS;
    s_pub[0] = 0;
}

static void draw(tui_surface *sf, tui_rect a)
{
    s_list_hit = s_status_hit = tui_rect_make(0, 0, 0, 0);
    s_bar_n = 0;
    poll_session();
    s_guess_cols = a.w;
    s_guess_rows = a.h - 1 - (s_bar_on ? 3 : 0);
    if (a.w < 24 || a.h < 8) { ls_panel_notice(sf, a, "SSH", "Enlarge the pane", NULL); return; }
    switch (s_view) {
    case V_HOSTS:   list_view(sf, a); break;
    case V_KEY:     key_view(sf, a); break;
    case V_CONNECT: case V_FAIL: connect_view(sf, a); break;
    case V_TERM:
        ls_tui_screen_set_bare(true);           /* every row is the remote's */
        term_view(sf, a);
        break;
    }
}

static bool term_key(ls_tk_t k, char ch)
{
    if (s_closed) {
        if (k == LS_TK_ENTER || k == LS_TK_ESC) { show_hosts(); return true; }
        return false;
    }
    char out[LS_SSH_KEY_MAX];
    int n = 0;
    s_note[0] = 0;
    switch (ls_ssh_key_encode(&s_mods, k, ch, s_vt && tmt_app_cursor_keys(s_vt), out, &n)) {
    case LS_SSH_KEY_SEND:       send(out, n); return true;
    case LS_SSH_KEY_HELD:       return true;
    case LS_SSH_KEY_DISCONNECT: disconnect(); return true;
    default:                    return false;   /* F-keys: the router's */
    }
}

static bool key(ls_tk_t k, char ch)
{
    switch (s_view) {
    case V_TERM:
        return term_key(k, ch);
    case V_HOSTS: {
        const int total = s_nhosts + 2;
        if (k == LS_TK_UP)   { s_sel = (s_sel + total - 1) % total; s_forget_armed = false; return true; }
        if (k == LS_TK_DOWN) { s_sel = (s_sel + 1) % total; s_forget_armed = false; return true; }
        if (k == LS_TK_ENTER) { open_selected(); return true; }
        if (k == LS_TK_CHAR && (ch == 'n' || ch == 'N')) { open_new(NULL); return true; }
        if (k == LS_TK_CHAR && (ch == 'k' || ch == 'K')) { s_pub[0] = 0; s_view = V_KEY; return true; }
        if (k == LS_TK_CHAR && (ch == 'f' || ch == 'F')) { forget_selected(); return true; }
        return false;                            /* ESC: home */
    }
    case V_KEY:
        if (k == LS_TK_ESC || k == LS_TK_BACKSPACE) { show_hosts(); return true; }
        return false;
    case V_CONNECT: case V_FAIL: {
        const ls_ssh_state_t st = s_sess ? ls_ssh_session_state(s_sess) : LS_SSH_CLOSED;
        if (st == LS_SSH_HOSTKEY_UNKNOWN) {
            /* No ENTER here: trusting a key should be a deliberate press. */
            if (k == LS_TK_CHAR && (ch == 't' || ch == 'T')) { trust(true); return true; }
            if (k == LS_TK_ESC || (k == LS_TK_CHAR && (ch == 'c' || ch == 'C'))) { trust(false); return true; }
            return true;
        }
        if (s_view == V_CONNECT) {
            if (k == LS_TK_ESC || (k == LS_TK_CHAR && (ch == 'c' || ch == 'C'))) { disconnect(); return true; }
            return true;
        }
        if (k == LS_TK_ESC || k == LS_TK_ENTER) { show_hosts(); return true; }
        if (k == LS_TK_CHAR && (ch == 'r' || ch == 'R')) { connect_button(1); return true; }
        if (k == LS_TK_CHAR && (ch == 'f' || ch == 'F' || ch == 'k' || ch == 'K')) { connect_button(2); return true; }
        return true;
    }
    }
    return false;
}

static bool touch(int col, int row)
{
    const int i = ls_btn_hit(col, row);
    if (i >= 0 && i < s_bar_n) {
        switch (s_view) {
        case V_TERM: term_button(i); break;
        case V_HOSTS:
            if (i == 0) open_selected();
            else if (i == 1) open_new(NULL);
            else if (i == 2) { s_pub[0] = 0; s_view = V_KEY; }
            else forget_selected();
            break;
        case V_KEY: if (i == 0) { s_keygen_armed = false; show_hosts(); } else keygen(); break;
        default: connect_button(i); break;
        }
        return true;
    }
    if (s_view == V_TERM && s_status_hit.w > 0 && tui_rect_contains(s_status_hit, col, row)) {
        if (s_closed) show_hosts();
        else s_bar_on = !s_bar_on;
        return true;
    }
    if (s_view == V_HOSTS && s_list_hit.w > 0 && tui_rect_contains(s_list_hit, col, row)) {
        const int at = s_list_top + (row - s_list_hit.y);
        if (at < s_nhosts + 2) {
            if (at == s_sel) open_selected();
            else { s_sel = at; s_forget_armed = false; }
        }
        return true;
    }
    return true;           /* a tap anywhere else is not ENTER */
}

const ls_tui_screen_t ls_scr_ssh = {
    .name  = "SSH",
    .hint  = "CTRL/ALT/FN then a key  FN Q disconnect  FN arrows PgUp/PgDn/Home/End  FN 1-0 F1-F10  F1-F8 leave (closes)",
    .enter = enter,
    .leave = leave,
    .draw  = draw,
    .key   = key,
    .touch = touch,
};

const ls_app_doc_t ls_doc_ssh = {
    .purpose = "A shell on a remote computer over Wi-Fi, logged in with a key "
               "made on and kept on the board. Servers are trusted on first use "
               "and a changed server key is refused.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

#endif
