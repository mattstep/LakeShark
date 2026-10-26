/* See ls_ssh_keys.h. */

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if !defined(ESP_PLATFORM) || (defined(CONFIG_LS_SSH_CLIENT) && CONFIG_LS_SSH_CLIENT)

#include "ls_ssh_keys.h"

#include <string.h>

int ls_ssh_ctrl_byte(char ch)
{
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 1;
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 1;
    /* xterm's table, digits included, since the keyboard's symbols sit on
       shifted letters and CTRL with shift is awkward on a thumb board. */
    switch (ch) {
    case '@': case ' ': case '2': return 0x00;
    case '[': case '3':           return 0x1b;
    case '\\': case '4':          return 0x1c;
    case ']': case '5':           return 0x1d;
    case '^': case '6':           return 0x1e;
    case '_': case '/': case '7': return 0x1f;
    case '?': case '8':           return 0x7f;
    default:                      return -1;
    }
}

static int put(char *out, const char *s)
{
    int n = (int)strlen(s);
    memcpy(out, s, (size_t)n);
    return n;
}

/* An arrow, with xterm's modifier form when CTRL or ALT is held. */
static int arrow(char *out, char dir, const ls_ssh_mods_t *m, bool app)
{
    if (m->ctrl || m->alt) {
        out[0] = 0x1b; out[1] = '['; out[2] = '1'; out[3] = ';';
        out[4] = m->ctrl ? (m->alt ? '7' : '5') : '3';
        out[5] = dir;
        return 6;
    }
    out[0] = 0x1b; out[1] = app ? 'O' : '['; out[2] = dir;
    return 3;
}

static ls_ssh_key_action_t fn_layer(ls_tk_t k, char ch, bool app, char *out, int *n)
{
    static const char *const FKEY[10] = {
        "\x1bOP", "\x1bOQ", "\x1bOR", "\x1bOS", "\x1b[15~",
        "\x1b[17~", "\x1b[18~", "\x1b[19~", "\x1b[20~", "\x1b[21~",
    };
    switch (k) {
    case LS_TK_UP:        *n = put(out, "\x1b[5~"); return LS_SSH_KEY_SEND;
    case LS_TK_DOWN:      *n = put(out, "\x1b[6~"); return LS_SSH_KEY_SEND;
    case LS_TK_LEFT:      *n = put(out, app ? "\x1bOH" : "\x1b[H"); return LS_SSH_KEY_SEND;
    case LS_TK_RIGHT:     *n = put(out, app ? "\x1bOF" : "\x1b[F"); return LS_SSH_KEY_SEND;
    case LS_TK_BACKSPACE: *n = put(out, "\x1b[3~"); return LS_SSH_KEY_SEND;
    case LS_TK_TAB:       *n = put(out, "\x1b[Z");  return LS_SSH_KEY_SEND;
    case LS_TK_CHAR:
        if (ch == 'q' || ch == 'Q') return LS_SSH_KEY_DISCONNECT;
        if (ch >= '1' && ch <= '9') { *n = put(out, FKEY[ch - '1']); return LS_SSH_KEY_SEND; }
        if (ch == '0')              { *n = put(out, FKEY[9]);        return LS_SSH_KEY_SEND; }
        return LS_SSH_KEY_PASS;      /* no meaning: fall back to the plain key */
    default:
        return LS_SSH_KEY_PASS;
    }
}

ls_ssh_key_action_t ls_ssh_key_encode(ls_ssh_mods_t *m, ls_tk_t k, char ch, bool app_cursor,
                                      char out[LS_SSH_KEY_MAX], int *n)
{
    *n = 0;
    switch (k) {
    case LS_TK_CTRL: m->ctrl = !m->ctrl; return LS_SSH_KEY_HELD;
    case LS_TK_ALT:  m->alt = !m->alt;   return LS_SSH_KEY_HELD;
    case LS_TK_FN:   m->fn = !m->fn;     return LS_SSH_KEY_HELD;
    default: break;
    }

    if (m->fn) {
        m->fn = false;
        ls_ssh_key_action_t a = fn_layer(k, ch, app_cursor, out, n);
        if (a != LS_SSH_KEY_PASS) { m->ctrl = m->alt = false; return a; }
    }

    char body[LS_SSH_KEY_MAX];
    int len = 0;
    switch (k) {
    case LS_TK_CHAR: {
        int c = (unsigned char)ch;
        if (m->ctrl) { int b = ls_ssh_ctrl_byte(ch); if (b >= 0) c = b; }
        body[len++] = (char)c;
        break;
    }
    case LS_TK_ENTER:     body[len++] = '\r'; break;
    case LS_TK_BACKSPACE: body[len++] = m->ctrl ? 0x08 : 0x7f; break;
    case LS_TK_TAB:       body[len++] = '\t'; break;
    case LS_TK_ESC:       body[len++] = 0x1b; break;
    case LS_TK_UP:    case LS_TK_DOWN:
    case LS_TK_LEFT:  case LS_TK_RIGHT: {
        const char dir = k == LS_TK_UP ? 'A' : k == LS_TK_DOWN ? 'B' : k == LS_TK_RIGHT ? 'C' : 'D';
        *n = arrow(out, dir, m, app_cursor);
        m->ctrl = m->alt = false;            /* already folded into the sequence */
        return LS_SSH_KEY_SEND;
    }
    default:
        return LS_SSH_KEY_PASS;              /* F-keys, META, MIC: the router's */
    }

    /* ALT is ESC first, as xterm's metaSendsEscape does it. */
    if (m->alt) out[(*n)++] = 0x1b;
    memcpy(out + *n, body, (size_t)len);
    *n += len;
    m->ctrl = m->alt = false;
    return LS_SSH_KEY_SEND;
}

#endif
