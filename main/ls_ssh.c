/* See ls_ssh.h. The console's side only: sessions and keys are in
   components/lsssh, and everything that reaches NVS happens here, on the
   console task, never on the session worker. */

#include "ls_ssh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ls_ssh_session.h"
#include "ls_ssh_store.h"

static const char *TAG = "ls_ssh";

#define SESSION_WAIT_MS   (10 * 60 * 1000)
#define POLL_MS           20
#define OUT_CHUNK         512

static void usage(void)
{
    puts("usage: ssh keygen [force]          make the board's Ed25519 identity");
    puts("       ssh pubkey                  print it for authorized_keys");
    puts("       ssh hosts | forget <host[:port]>");
    puts("       ssh [-v] [-y] [-p port] user@host [command ...]");
    puts("  -y   trust an unknown host key and pin it (shown first without -y)");
    puts("  -v   libssh protocol log");
}

static int do_keygen(bool force)
{
    if (ls_ssh_store_has_identity() && !force) {
        puts("ssh: an identity exists; 'ssh keygen force' replaces it (servers will need the new key)");
        return 1;
    }
    char *priv = NULL, pub[160], err[96];
    if (!ls_ssh_keygen(&priv, pub, sizeof(pub), err, sizeof(err))) {
        printf("ssh: %s\n", err);
        return 1;
    }
    esp_err_t e = ls_ssh_store_set_identity(priv);
    ls_ssh_free_secret(priv);
    if (e != ESP_OK) { printf("ssh: NVS write failed: %s\n", esp_err_to_name(e)); return 1; }
    printf("ssh: new identity stored. Add this line to ~/.ssh/authorized_keys on your servers:\n%s\n", pub);
    return 0;
}

static int do_pubkey(void)
{
    char *b64 = ls_ssh_store_identity();
    if (!b64) { puts("ssh: no identity yet - run 'ssh keygen'"); return 1; }
    char pub[160];
    bool ok = ls_ssh_pubkey_line(b64, pub, sizeof(pub));
    ls_ssh_free_secret(b64);
    if (!ok) { puts("ssh: stored identity is unreadable - 'ssh keygen force' makes a new one"); return 1; }
    printf("%s\n", pub);
    return 0;
}

static void print_record(const char *rec, void *ctx) { (void)ctx; printf("%s\n", rec); }

static int do_hosts(void)
{
    if (!ls_ssh_store_each_record(print_record, NULL)) puts("ssh: no pinned hosts");
    return 0;
}

static int do_forget(const char *spec)
{
    char host[128];
    unsigned port = 22;
    ls_ssh_split_hostport(spec, host, sizeof(host), &port);
    esp_err_t err = ls_ssh_store_forget(host, port);
    if (err == ESP_ERR_NOT_FOUND) { printf("ssh: %s:%u was not pinned\n", host, port); return 1; }
    printf("ssh: %s:%u %s\n", host, port, err == ESP_OK ? "forgotten" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* Copies what the remote printed to the console. */
static void drain(ls_ssh_session_t *s, char *buf)
{
    size_t n;
    while ((n = ls_ssh_session_read(s, buf, OUT_CHUNK)) > 0) fwrite(buf, 1, n, stdout);
}

static int do_run(int argc, char **argv)
{
    char host[128], user[64];
    unsigned port = 22;
    bool trust_new = false, verbose = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; ++i) {
        if (!strcmp(argv[i], "-y")) trust_new = true;
        else if (!strcmp(argv[i], "-v")) verbose = true;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = (unsigned)atoi(argv[++i]);
        else { usage(); return 1; }
    }
    if (i >= argc) { usage(); return 1; }
    const char *at = strchr(argv[i], '@');
    if (!at || at == argv[i] || !at[1]) { puts("ssh: give user@host"); return 1; }
    snprintf(user, sizeof(user), "%.*s", (int)(at - argv[i]), argv[i]);
    ls_ssh_split_hostport(at + 1, host, sizeof(host), &port);
    ++i;

    /* Remaining words are the remote command. */
    char *command = NULL;
    if (i < argc) {
        size_t n = 0;
        for (int k = i; k < argc; ++k) n += strlen(argv[k]) + 1;
        command = malloc(n);
        if (!command) return 1;
        command[0] = 0;
        for (int k = i; k < argc; ++k) { strcat(command, argv[k]); if (k + 1 < argc) strcat(command, " "); }
    }

    char *identity = ls_ssh_store_identity();
    if (!identity) {
        puts("ssh: no identity yet - run 'ssh keygen' and add the key to the server first");
        free(command);
        return 1;
    }
    char *pinned_rec = ls_ssh_store_host_record(host, port);    /* "host:port <type> <b64>" */

    printf("ssh: %s@%s:%u%s%s\n", user, host, port, command ? " -- " : "", command ? command : "");
    const ls_ssh_params_t p = {
        .host = host, .port = port, .user = user,
        .identity = identity, .pinned = ls_ssh_store_record_key(pinned_rec),
        .trust_new = trust_new, .verbose = verbose,
        .mode = LS_SSH_EXEC, .command = command,
    };
    char why[96];
    ls_ssh_session_t *s = ls_ssh_session_open(&p, why, sizeof(why));
    ls_ssh_free_secret(identity);            /* the session has its own copy */
    free(pinned_rec);
    free(command);
    if (!s) { printf("ssh: %s\n", why); return 1; }

    char *buf = malloc(OUT_CHUNK);
    bool shown_fp = false;
    int waited = 0;
    ls_ssh_info_t info;
    for (;;) {
        ls_ssh_state_t st = ls_ssh_session_state(s);
        if (!shown_fp && st != LS_SSH_CONNECTING) {
            ls_ssh_session_info(s, &info);
            if (info.fingerprint[0]) printf("ssh: host key %s %s\n", info.key_type, info.fingerprint);
            shown_fp = true;
        }
        /* Unknown, and no -y: the fingerprint is on the screen; the user
           repeats the command with -y once it checks out. */
        if (st == LS_SSH_HOSTKEY_UNKNOWN) ls_ssh_session_trust(s, false);
        if (buf) drain(s, buf);
        if (st == LS_SSH_CLOSED) break;
        if (waited >= SESSION_WAIT_MS) {
            ESP_LOGW(TAG, "session still running after %d s; closing", SESSION_WAIT_MS / 1000);
            ls_ssh_session_close(s);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        waited += POLL_MS;
    }
    if (buf) { drain(s, buf); free(buf); }
    fflush(stdout);
    ls_ssh_session_info(s, &info);
    printf("ssh: heap internal %u->%u (low-water %u->%u) psram %u->%u, stack high-water %u\n",
           info.heap_int0, info.heap_int1, info.heap_low0, info.heap_low1,
           info.heap_ext0, info.heap_ext1, info.stack_free);

    const bool ok = info.end == LS_SSH_END_EXIT;
    if (ok && info.server_key_new && ls_ssh_session_server_key(s)) {
        esp_err_t err = ls_ssh_store_pin(host, port, ls_ssh_session_server_key(s));
        printf("ssh: host key for %s:%u %s\n", host, port, err == ESP_OK ? "pinned" : esp_err_to_name(err));
    }
    printf("ssh: %s\n", info.message);
    ls_ssh_session_free(s);
    return ok ? 0 : 1;
}

static int cmd_ssh(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "keygen")) return do_keygen(argc > 2 && !strcmp(argv[2], "force"));
    if (!strcmp(argv[1], "pubkey")) return do_pubkey();
    if (!strcmp(argv[1], "hosts"))  return do_hosts();
    if (!strcmp(argv[1], "forget")) { if (argc < 3) { usage(); return 1; } return do_forget(argv[2]); }
    if (!strcmp(argv[1], "help"))   { usage(); return 0; }
    return do_run(argc, argv);
}

void ls_ssh_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "ssh",
        .help = "SSH client with the board's Ed25519 key: ssh keygen | pubkey | hosts | forget | [-y] [-p N] user@host [cmd]",
        .func = &cmd_ssh,
    };
    esp_err_t err = esp_console_cmd_register(&cmd);
    if (err != ESP_OK) ESP_LOGW(TAG, "console command not registered: %s", esp_err_to_name(err));
}
