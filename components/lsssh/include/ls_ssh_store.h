/* What SSH keeps in NVS, namespace "lsssh":
     id_ed25519   the board's private key, OpenSSH format, base64, unencrypted
     h<hash>      "host:port <type> <base64>", one pinned server key per host
     u<hash>      the user last logged in as on that host (the SSH app's list)
   <hash> is FNV-1a of "host:port" (host lower-cased), because NVS keys are
   15 characters at most.

   ONLY CALL THESE FROM A TASK WHOSE STACK IS IN INTERNAL RAM (the console,
   the UI task). An NVS write turns the flash cache off, and a PSRAM stack
   with it - which is why the session worker never comes here. */

#ifndef LS_SSH_STORE_H
#define LS_SSH_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The identity, or NULL. Free with ls_ssh_free_secret. */
char     *ls_ssh_store_identity(void);
esp_err_t ls_ssh_store_set_identity(const char *b64);
bool      ls_ssh_store_has_identity(void);

/* The pinned record "host:port <type> <b64>", or NULL; caller frees. */
char       *ls_ssh_store_host_record(const char *host, unsigned port);
/* The "<type> <b64>" part of a record, or NULL. Points into rec. */
const char *ls_ssh_store_record_key(const char *rec);
esp_err_t   ls_ssh_store_pin(const char *host, unsigned port, const char *keyline);
/* Drops the key and the remembered user. ESP_ERR_NOT_FOUND if not pinned. */
esp_err_t   ls_ssh_store_forget(const char *host, unsigned port);

/* Remembered user for a host, into out; false when there is none. */
bool      ls_ssh_store_user(const char *host, unsigned port, char *out, size_t n);
esp_err_t ls_ssh_store_set_user(const char *host, unsigned port, const char *user);

typedef struct {
    char     host[64];
    unsigned port;
    char     user[32];       /* "" when none was remembered */
} ls_ssh_host_t;

/* Pinned hosts, in NVS order. Returns how many were written. */
int  ls_ssh_store_hosts(ls_ssh_host_t *out, int cap);
/* Every pinned record, for 'ssh hosts'. Returns how many. */
int  ls_ssh_store_each_record(void (*fn)(const char *rec, void *ctx), void *ctx);

/* "host[:port]" -> host, port (unchanged when no port is given). A host with
   more than one colon is an IPv6 literal and keeps them. */
void ls_ssh_split_hostport(const char *in, char *host, size_t cap, unsigned *port);

#ifdef __cplusplus
}
#endif

#endif
