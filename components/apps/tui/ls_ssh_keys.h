/* Router keys to the bytes a remote terminal expects, for the SSH app.

   The T-Display-P4 keyboard sends CTRL, ALT and FN as keys of their own, on
   press, with no state the screen could read (compact_ui.cpp keeps only
   shift and caps). So they are sticky here: press CTRL, then C, and the
   remote gets 0x03. Pressing the same modifier again cancels it.

   FN is this app's own layer, because ESC has to reach the remote shell and
   so cannot be the way out:
     FN Q          disconnect
     FN UP/DOWN    Page Up / Page Down
     FN LEFT/RIGHT Home / End
     FN BACKSPACE  Delete
     FN TAB        Back-tab
     FN 1..9, 0    F1..F10 on the remote (the real F-keys stay the router's)

   Pure C with no device dependencies, so the bench tests it. */

#ifndef LS_SSH_KEYS_H
#define LS_SSH_KEYS_H

#include <stdbool.h>

#include "ls_tui_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ctrl, alt, fn;
} ls_ssh_mods_t;

typedef enum {
    LS_SSH_KEY_PASS = 0,     /* not ours: let the router have it (F-keys)  */
    LS_SSH_KEY_SEND,         /* out[0..*n) goes to the remote              */
    LS_SSH_KEY_HELD,         /* a modifier changed; nothing to send        */
    LS_SSH_KEY_DISCONNECT,   /* FN Q                                       */
} ls_ssh_key_action_t;

#define LS_SSH_KEY_MAX 16

/* app_cursor: the remote asked for application cursor keys (DECCKM), so
   arrows are ESC O x rather than ESC [ x. */
ls_ssh_key_action_t ls_ssh_key_encode(ls_ssh_mods_t *m, ls_tk_t k, char ch, bool app_cursor,
                                      char out[LS_SSH_KEY_MAX], int *n);

/* The control byte for CTRL+ch, or -1 when CTRL does nothing to it. */
int ls_ssh_ctrl_byte(char ch);

#ifdef __cplusplus
}
#endif

#endif
