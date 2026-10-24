/* One SSH session on its own task: connect, check the host key, log in with
   the board's Ed25519 key, then run a command or a shell with a pty.

   The 'ssh' console command and the SSH app are both clients of this.

   The worker task does network and crypto only. Its stack is in PSRAM, and a
   task on a PSRAM stack must never touch flash (no NVS, SPIFFS, OTA): a flash
   operation turns the cache off and the stack goes with it. So the identity
   and the pinned key come IN as strings, and a newly trusted key comes OUT
   as a string for the caller to pin from its own task (ls_ssh_store.h).

   The caller's side of the API is polled and never blocks for long, so the
   UI can call it from draw(): ls_ssh_session_state() for where it is,
   ls_ssh_session_read() for what the remote printed (a PSRAM ring the
   worker fills), ls_ssh_session_write() for keystrokes (a small ring the
   worker drains). All of these are safe from one caller task at a time. */

#ifndef LS_SSH_SESSION_H
#define LS_SSH_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_SSH_EXEC = 0,     /* run `command`, collect its output and exit status */
    LS_SSH_SHELL,        /* pty of cols x rows, then the login shell          */
} ls_ssh_mode_t;

typedef struct {
    const char *host;
    unsigned    port;            /* 0 means 22 */
    const char *user;
    /* OpenSSH private key, base64, as ls_ssh_store_identity() returns it.
       The session takes its own copy and scrubs it; the caller may wipe
       theirs as soon as open returns. */
    const char *identity;
    /* "<type> <base64>" pinned for this host, or NULL when unknown. */
    const char *pinned;
    /* An unknown host key is accepted without asking (console -y). Without
       it the session stops in LS_SSH_HOSTKEY_UNKNOWN until ls_ssh_session_trust. */
    bool        trust_new;
    bool        verbose;         /* libssh protocol log on the console */

    ls_ssh_mode_t mode;
    const char *command;         /* EXEC */
    int         cols, rows;      /* SHELL */
    const char *term;            /* SHELL; NULL means "xterm" */
} ls_ssh_params_t;

typedef enum {
    LS_SSH_CONNECTING = 0,       /* resolving, TCP, key exchange           */
    LS_SSH_HOSTKEY_UNKNOWN,      /* waiting for ls_ssh_session_trust()     */
    LS_SSH_AUTHENTICATING,
    LS_SSH_OPEN,                 /* the channel is up; read and write      */
    LS_SSH_CLOSED,               /* see ls_ssh_info_t.end                  */
} ls_ssh_state_t;

typedef enum {
    LS_SSH_END_NONE = 0,
    LS_SSH_END_EXIT,             /* the remote side finished; exit_status  */
    LS_SSH_END_LOCAL,            /* ls_ssh_session_close()                 */
    LS_SSH_END_CONNECT,          /* resolve/TCP/kex failed                 */
    LS_SSH_END_HOSTKEY_CHANGED,  /* differs from the pinned key: refused   */
    LS_SSH_END_HOSTKEY_REFUSED,  /* unknown, and not trusted               */
    LS_SSH_END_AUTH,             /* the key was refused, or unreadable     */
    LS_SSH_END_CHANNEL,          /* channel, pty, shell or exec refused    */
    LS_SSH_END_IO,               /* the connection broke                   */
    LS_SSH_END_NOMEM,
} ls_ssh_end_t;

typedef struct {
    ls_ssh_state_t state;
    ls_ssh_end_t   end;
    int            exit_status;  /* -1 until known */
    char           message[160]; /* why it closed, in words                */
    char           fingerprint[64]; /* "SHA256:..." once the key is seen   */
    char           key_type[24];    /* "ssh-ed25519" ...                   */
    bool           server_key_new;  /* trusted this time: pin it           */
    /* Measured by the worker, for the console's report. */
    unsigned       heap_int0, heap_int1, heap_low0, heap_low1;
    unsigned       heap_ext0, heap_ext1, stack_free;
} ls_ssh_info_t;

typedef struct ls_ssh_session ls_ssh_session_t;

/* Start the worker. NULL when there is no memory for it or a task; the
   reason is in `why` when that is not NULL. */
ls_ssh_session_t *ls_ssh_session_open(const ls_ssh_params_t *p, char *why, size_t why_n);

ls_ssh_state_t ls_ssh_session_state(const ls_ssh_session_t *s);
/* A consistent copy of everything above. */
void ls_ssh_session_info(ls_ssh_session_t *s, ls_ssh_info_t *out);
/* "<type> <base64>" of the server key once seen, else NULL. Valid until
   ls_ssh_session_free. */
const char *ls_ssh_session_server_key(const ls_ssh_session_t *s);

/* In LS_SSH_HOSTKEY_UNKNOWN: go on (and set server_key_new), or stop with
   LS_SSH_END_HOSTKEY_REFUSED. */
void ls_ssh_session_trust(ls_ssh_session_t *s, bool accept);

/* Remote output, oldest first; returns bytes copied, 0 when none. stdout and
   stderr arrive interleaved, as a terminal shows them. */
size_t ls_ssh_session_read(ls_ssh_session_t *s, void *buf, size_t cap);
/* Bytes for the remote side; returns how many were queued (fewer when the
   input ring is full). Ignored unless OPEN. */
size_t ls_ssh_session_write(ls_ssh_session_t *s, const void *bytes, size_t n);
/* New pty size; applied by the worker. */
void ls_ssh_session_resize(ls_ssh_session_t *s, int cols, int rows);
/* Ask the worker to stop. Returns at once; it reaches CLOSED shortly (a
   connect in progress finishes its timeout first). */
void ls_ssh_session_close(ls_ssh_session_t *s);
/* Close and let go. Never blocks on the network: a worker still inside
   libssh frees the session itself when it gets out. */
void ls_ssh_session_free(ls_ssh_session_t *s);

/* ------------------------------------------------------ key operations ---
   Run on a short-lived worker (crypto wants the stack) and wait for it. */

/* A new Ed25519 identity. *priv_b64 is the OpenSSH private key in base64
   (free it with ls_ssh_free_secret); pub_line gets "ssh-ed25519 <b64>
   <comment>". Returns false with the reason in err. */
bool ls_ssh_keygen(char **priv_b64, char *pub_line, size_t pub_n, char *err, size_t err_n);
/* The public half of a stored identity as an authorized_keys line. */
bool ls_ssh_pubkey_line(const char *priv_b64, char *pub_line, size_t pub_n);

/* Scrub key material; a plain memset to a dying buffer is optimised away. */
void ls_ssh_wipe(void *p, size_t n);
/* Wipe a NUL-terminated secret and free it. NULL is fine. */
void ls_ssh_free_secret(char *s);

#ifdef __cplusplus
}
#endif

#endif
