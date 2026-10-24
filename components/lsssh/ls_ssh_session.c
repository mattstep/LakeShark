/* See ls_ssh_session.h. */

#include "ls_ssh_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include <libssh/libssh.h>

static const char *TAG = "ls_ssh";

#define WORKER_STACK      24576   /* key exchange + auth + exec peaked at ~13.5 KB */
#define WORKER_PRIO       5
#define CONNECT_TIMEOUT_S 15
#define TRUST_WAIT_MS     (120 * 1000)  /* sshd's LoginGraceTime */
#define OUT_RING          (24 * 1024)
#define IN_RING           1024
#define CHUNK             512
#define IDLE_POLL_MS      20      /* longest a keystroke waits for the worker */
#define JOB_WAIT_MS       30000
/* A path prefix with no VFS behind it; see ensure_init(). */
#define LS_SSH_NOFS       "/nossh"

struct ls_ssh_session {
    char      host[128], user[64];
    unsigned  port;
    char     *identity;          /* scrubbed as soon as it is imported */
    char     *pinned;
    char     *command;
    char      term[32];
    bool      trust_new, verbose;
    ls_ssh_mode_t mode;

    volatile int  cols, rows;
    volatile bool resize_req, close_req;
    volatile int  trust;         /* 0 undecided, 1 accept, -1 refuse */

    StreamBufferHandle_t out;    /* worker -> caller, PSRAM */
    StreamBufferHandle_t in;     /* caller -> worker */

    ls_ssh_info_t info;          /* under s_mux */
    char         *server_key;    /* set once, before state passes CONNECTING */

    TaskHandle_t task;
    bool done, detached;         /* under s_mux: who frees the session */
};

/* One lock for every session's small shared fields. Static rather than per
   session because the session itself lives in PSRAM and a spinlock wants
   internal RAM; nothing holds it for longer than a struct copy. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* libssh's own init, once per boot; ssh_init is reference counted and is
   never undone, since the next session would only redo it. */
static bool s_ssh_ready;

/* ----------------------------------------------------------- helpers --- */

void ls_ssh_wipe(void *p, size_t n) { volatile char *v = p; while (n--) *v++ = 0; }

void ls_ssh_free_secret(char *s)
{
    if (!s) return;
    ls_ssh_wipe(s, strlen(s));
    free(s);
}

static char *dup_str(const char *s)
{
    return s ? strdup(s) : NULL;
}

static bool ensure_init(void)
{
    if (s_ssh_ready) return true;
    /* libssh expands "%d" and "~" from the password database, which this
       port stubs out, and then from HOME. Give it one before anything else.
       It must NOT be a flash-backed mount: the worker's stack is in PSRAM, and
       even a failed open() on SPIFFS reads flash, which asserts with the
       caches off (seen as an illegal-instruction panic in panic_abort). A
       prefix no VFS owns fails in esp_vfs_open with ENOENT and touches nothing. */
    setenv("HOME", LS_SSH_NOFS, 1);
    if (ssh_init() != SSH_OK) return false;
    s_ssh_ready = true;
    return true;
}

static void board_comment(char *out, size_t cap)
{
    uint8_t mac[6] = {0};
    /* The Wi-Fi MAC belongs to the C6; the P4 has its own factory MAC in eFuse. */
    esp_efuse_mac_get_default(mac);
    snprintf(out, cap, "lakeshark-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

/* "<type> <base64>" for a key, caller frees. */
static char *key_line(ssh_key key)
{
    char *b64 = NULL;
    if (ssh_pki_export_pubkey_base64(key, &b64) != SSH_OK || !b64) return NULL;
    const char *type = ssh_key_type_to_char(ssh_key_type(key));
    size_t n = strlen(type) + 1 + strlen(b64) + 1;
    char *line = malloc(n);
    if (line) snprintf(line, n, "%s %s", type, b64);
    ssh_string_free_char(b64);
    return line;
}

static void fingerprint(ssh_key key, char *out, size_t cap)
{
    unsigned char *hash = NULL;
    size_t hlen = 0;
    out[0] = 0;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) != 0) return;
    char *fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
    if (fp) { snprintf(out, cap, "%s", fp); ssh_string_free_char(fp); }
    ssh_clean_pubkey_hash(&hash);
}

static void set_state(ls_ssh_session_t *s, ls_ssh_state_t st)
{
    portENTER_CRITICAL(&s_mux);
    s->info.state = st;
    portEXIT_CRITICAL(&s_mux);
}

/* Formatted outside the lock; copied under it. */
static void set_end(ls_ssh_session_t *s, ls_ssh_end_t end, const char *fmt, ...)
{
    char msg[sizeof(s->info.message)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&s_mux);
    s->info.end = end;
    memcpy(s->info.message, msg, sizeof(msg));
    portEXIT_CRITICAL(&s_mux);
}

/* ------------------------------------------------------------ worker --- */

/* The channel is up: shuttle bytes until either side stops. */
static void pump(ls_ssh_session_t *s, ssh_session ss, ssh_channel ch)
{
    char buf[CHUNK];
    for (;;) {
        if (s->close_req) { set_end(s, LS_SSH_END_LOCAL, "closed"); return; }
        bool progress = false, full = false;

        if (s->resize_req) {
            s->resize_req = false;
            ssh_channel_change_pty_size(ch, s->cols, s->rows);
        }

        size_t k = xStreamBufferReceive(s->in, buf, sizeof(buf), 0);
        if (k) {
            if (ssh_channel_write(ch, buf, (uint32_t)k) == SSH_ERROR) {
                set_end(s, LS_SSH_END_IO, "write: %s", ssh_get_error(ss));
                return;
            }
            progress = true;
        }

        for (int is_stderr = 0; is_stderr < 2; ++is_stderr) {
            size_t room = xStreamBufferSpacesAvailable(s->out);
            if (!room) { full = true; break; }      /* the reader is behind: leave it in libssh */
            int n = ssh_channel_read_nonblocking(ch, buf, (uint32_t)(room < sizeof(buf) ? room : sizeof(buf)), is_stderr);
            if (n == SSH_ERROR) { set_end(s, LS_SSH_END_IO, "read: %s", ssh_get_error(ss)); return; }
            if (n > 0) { xStreamBufferSend(s->out, buf, (size_t)n, 0); progress = true; }
        }

        if (!full) {
            int a = ssh_channel_poll(ch, 0), b = ssh_channel_poll(ch, 1);
            if (a == SSH_ERROR || b == SSH_ERROR) { set_end(s, LS_SSH_END_IO, "connection: %s", ssh_get_error(ss)); return; }
            if ((a == SSH_EOF && b == SSH_EOF) || ssh_channel_is_closed(ch)) break;
        }
        if (!progress) {
            if (full) vTaskDelay(pdMS_TO_TICKS(10));
            else ssh_channel_poll_timeout(ch, IDLE_POLL_MS, 0);
        }
    }

    /* The remote side is done: collect how it ended. */
    ssh_channel_send_eof(ch);
    uint32_t code = 0;
    char *sig = NULL;
    int core = 0;
    if (ssh_channel_get_exit_state(ch, &code, &sig, &core) == SSH_OK) {
        portENTER_CRITICAL(&s_mux);
        s->info.exit_status = sig ? -1 : (int)code;
        portEXIT_CRITICAL(&s_mux);
        if (sig) set_end(s, LS_SSH_END_EXIT, "killed by signal %s", sig);
        else set_end(s, LS_SSH_END_EXIT, "exit status %d", (int)code);
    } else {
        set_end(s, LS_SSH_END_EXIT, "remote end closed");
    }
    if (sig) ssh_string_free_char(sig);
}

static void run_session(ls_ssh_session_t *s)
{
    ssh_session ss = ssh_new();
    ssh_key identity = NULL, server = NULL;
    ssh_channel ch = NULL;
    if (!ss) { set_end(s, LS_SSH_END_NOMEM, "no memory for a session"); return; }

    long timeout = CONNECT_TIMEOUT_S;
    /* NOLOG by default: at WARNING libssh reports every known_hosts file it
       cannot open, and on this board none of them exist by design. */
    int verbosity = s->verbose ? SSH_LOG_PROTOCOL : SSH_LOG_NOLOG;
    int no = 0;
    /* An explicit directory: the default is "~/.ssh", and the port has no
       password database to expand "~" with, so ssh_connect would refuse. */
    ssh_options_set(ss, SSH_OPTIONS_SSH_DIR, LS_SSH_NOFS "/ssh");
    ssh_options_set(ss, SSH_OPTIONS_HOST, s->host);
    ssh_options_set(ss, SSH_OPTIONS_PORT, &s->port);
    ssh_options_set(ss, SSH_OPTIONS_USER, s->user);
    ssh_options_set(ss, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(ss, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    /* No ssh_config and no known_hosts files: trust lives in NVS. */
    ssh_options_set(ss, SSH_OPTIONS_PROCESS_CONFIG, &no);
    ssh_options_set(ss, SSH_OPTIONS_KNOWNHOSTS, LS_SSH_NOFS "/known_hosts");
    ssh_options_set(ss, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, LS_SSH_NOFS "/ssh_known_hosts");

    if (ssh_connect(ss) != SSH_OK) {
        set_end(s, LS_SSH_END_CONNECT, "connect: %s", ssh_get_error(ss));
        goto out;
    }

    /* Host key: pinned, new, or changed. */
    if (ssh_get_server_publickey(ss, &server) != SSH_OK) {
        set_end(s, LS_SSH_END_CONNECT, "no server key: %s", ssh_get_error(ss));
        goto out;
    }
    char fp[sizeof(s->info.fingerprint)];
    fingerprint(server, fp, sizeof(fp));
    char *line = key_line(server);
    if (!line) { set_end(s, LS_SSH_END_NOMEM, "server key export failed"); goto out; }
    portENTER_CRITICAL(&s_mux);
    s->server_key = line;
    memcpy(s->info.fingerprint, fp, sizeof(fp));
    snprintf(s->info.key_type, sizeof(s->info.key_type), "%s", ssh_key_type_to_char(ssh_key_type(server)));
    portEXIT_CRITICAL(&s_mux);

    if (s->pinned) {
        if (strcmp(s->pinned, line) != 0) {
            set_end(s, LS_SSH_END_HOSTKEY_CHANGED,
                    "HOST KEY CHANGED for %s:%u - refusing. 'ssh forget %s' if this is expected",
                    s->host, s->port, s->host);
            goto out;
        }
    } else if (!s->trust_new) {
        set_state(s, LS_SSH_HOSTKEY_UNKNOWN);
        for (int waited = 0; !s->trust && !s->close_req && waited < TRUST_WAIT_MS; waited += 50)
            vTaskDelay(pdMS_TO_TICKS(50));
        if (s->close_req) { set_end(s, LS_SSH_END_LOCAL, "closed"); goto out; }
        if (s->trust <= 0) {
            set_end(s, LS_SSH_END_HOSTKEY_REFUSED,
                    "unknown host %s:%u - check the fingerprint above, then repeat with -y to trust it",
                    s->host, s->port);
            goto out;
        }
    }
    if (!s->pinned) {
        portENTER_CRITICAL(&s_mux);
        s->info.server_key_new = true;
        portEXIT_CRITICAL(&s_mux);
    }

    /* Public key auth with the board identity. */
    set_state(s, LS_SSH_AUTHENTICATING);
    int imported = ssh_pki_import_privkey_base64(s->identity, NULL, NULL, NULL, &identity);
    ls_ssh_free_secret(s->identity);         /* the key object has it now */
    s->identity = NULL;
    if (imported != SSH_OK || !identity) {
        set_end(s, LS_SSH_END_AUTH, "stored identity is unreadable - 'ssh keygen force' makes a new one");
        goto out;
    }
    int auth = ssh_userauth_publickey(ss, NULL, identity);
    if (auth != SSH_AUTH_SUCCESS) {
        set_end(s, LS_SSH_END_AUTH, "public key refused for %s@%s (%s) - is 'ssh pubkey' in authorized_keys?",
                s->user, s->host, auth == SSH_AUTH_DENIED ? "denied" : ssh_get_error(ss));
        goto out;
    }

    if (s->mode == LS_SSH_EXEC && !s->command) {
        set_end(s, LS_SSH_END_EXIT, "authenticated. An interactive shell comes with the terminal app; give a command for now");
        goto out;
    }

    ch = ssh_channel_new(ss);
    if (!ch || ssh_channel_open_session(ch) != SSH_OK) {
        set_end(s, LS_SSH_END_CHANNEL, "channel: %s", ssh_get_error(ss));
        goto out;
    }
    if (s->mode == LS_SSH_SHELL) {
        if (ssh_channel_request_pty_size(ch, s->term, s->cols, s->rows) != SSH_OK) {
            set_end(s, LS_SSH_END_CHANNEL, "pty: %s", ssh_get_error(ss));
            goto out;
        }
        s->resize_req = false;               /* the request carried the size */
        if (ssh_channel_request_shell(ch) != SSH_OK) {
            set_end(s, LS_SSH_END_CHANNEL, "shell: %s", ssh_get_error(ss));
            goto out;
        }
    } else if (ssh_channel_request_exec(ch, s->command) != SSH_OK) {
        set_end(s, LS_SSH_END_CHANNEL, "exec: %s", ssh_get_error(ss));
        goto out;
    }
    set_state(s, LS_SSH_OPEN);
    pump(s, ss, ch);

out:
    /* Every path frees everything libssh handed out, in reverse order. */
    if (ch) { ssh_channel_close(ch); ssh_channel_free(ch); }
    if (identity) ssh_key_free(identity);
    if (server) ssh_key_free(server);
    if (ssh_is_connected(ss)) ssh_disconnect(ss);
    ssh_free(ss);
}

/* ESP-IDF 5.5.4's vStreamBufferDeleteWithCaps() (freertos/esp_additions/
   idf_additions.c, vStreamBufferGenericDeleteWithCaps) deletes a stream
   buffer with vSemaphoreDelete(), which reads the handle as a queue, decides
   it was allocated dynamically and frees the control block; the helper then
   frees that block a second time. Seen on hardware as "block already marked
   as free" in tlsf_free at every session teardown. Take the two blocks out
   first and delete the buffer with the kernel's own call. */
static void stream_buffer_delete(StreamBufferHandle_t sb)
{
    StaticStreamBuffer_t *ctl = NULL;
    uint8_t *storage = NULL;
    if (xStreamBufferGetStaticBuffers(sb, &storage, &ctl) != pdTRUE) return;
    vStreamBufferDelete(sb);
    heap_caps_free(ctl);
    heap_caps_free(storage);
}

static void destroy(ls_ssh_session_t *s)
{
    ls_ssh_free_secret(s->identity);
    free(s->pinned);
    free(s->command);
    free(s->server_key);
    if (s->out) stream_buffer_delete(s->out);
    if (s->in) stream_buffer_delete(s->in);
    heap_caps_free(s);
}

static void session_task(void *arg)
{
    ls_ssh_session_t *s = arg;
    const unsigned int0 = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const unsigned ext0 = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const unsigned low0 = (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    if (!ensure_init()) set_end(s, LS_SSH_END_NOMEM, "libssh init failed");
    else run_session(s);

    bool detached;
    portENTER_CRITICAL(&s_mux);
    s->info.heap_int0 = int0;
    s->info.heap_ext0 = ext0;
    s->info.heap_low0 = low0;
    portEXIT_CRITICAL(&s_mux);
    const unsigned int1 = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const unsigned ext1 = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const unsigned low1 = (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const unsigned stk = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&s_mux);
    s->info.heap_int1 = int1;
    s->info.heap_ext1 = ext1;
    s->info.heap_low1 = low1;
    s->info.stack_free = stk;
    s->info.state = LS_SSH_CLOSED;
    s->done = true;
    detached = s->detached;
    portEXIT_CRITICAL(&s_mux);

    if (detached) {
        /* The owner has gone: free the session, and the task's own PSRAM
           stack and TCB. A plain vTaskDelete would leak both, since a
           WithCaps task's memory is only returned by vTaskDeleteWithCaps. */
        destroy(s);
        vTaskDeleteWithCaps(NULL);
    }
    /* The owner deletes this task (and its stack) in ls_ssh_session_free. */
    for (;;) vTaskSuspend(NULL);
}

/* PSRAM stack first - internal RAM is the scarce one - internal if PSRAM
   stacks are refused. Whatever runs here must never touch flash: no NVS,
   no SPIFFS, no OTA. A flash operation turns the cache off and a PSRAM
   stack goes with it. */
static bool start_worker(TaskFunction_t fn, void *arg, TaskHandle_t *out)
{
    BaseType_t ok = xTaskCreateWithCaps(fn, "ssh", WORKER_STACK, arg, WORKER_PRIO, out,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS)
        ok = xTaskCreateWithCaps(fn, "ssh", WORKER_STACK, arg, WORKER_PRIO, out,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return ok == pdPASS;
}

/* ---------------------------------------------------- the caller's side --- */

ls_ssh_session_t *ls_ssh_session_open(const ls_ssh_params_t *p, char *why, size_t why_n)
{
    char dummy[4];
    if (!why) { why = dummy; why_n = sizeof(dummy); }
    why[0] = 0;
    if (!p || !p->host || !p->user || !p->identity) { snprintf(why, why_n, "missing host, user or identity"); return NULL; }

    ls_ssh_session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) { snprintf(why, why_n, "no memory"); return NULL; }
    snprintf(s->host, sizeof(s->host), "%s", p->host);
    snprintf(s->user, sizeof(s->user), "%s", p->user);
    snprintf(s->term, sizeof(s->term), "%s", p->term ? p->term : "xterm");
    s->port = p->port ? p->port : 22;
    s->identity = dup_str(p->identity);
    s->pinned = dup_str(p->pinned);
    s->command = dup_str(p->command);
    s->trust_new = p->trust_new;
    s->verbose = p->verbose;
    s->mode = p->mode;
    s->cols = p->cols > 0 ? p->cols : 80;
    s->rows = p->rows > 0 ? p->rows : 24;
    s->info.exit_status = -1;
    s->out = xStreamBufferCreateWithCaps(OUT_RING, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->in = xStreamBufferCreateWithCaps(IN_RING, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->identity || (p->pinned && !s->pinned) || (p->command && !s->command) || !s->out || !s->in) {
        destroy(s);
        snprintf(why, why_n, "no memory");
        return NULL;
    }
    if (!start_worker(session_task, s, &s->task)) {
        destroy(s);
        snprintf(why, why_n, "could not start the ssh task (%u bytes of stack)", WORKER_STACK);
        return NULL;
    }
    return s;
}

ls_ssh_state_t ls_ssh_session_state(const ls_ssh_session_t *s)
{
    return s ? s->info.state : LS_SSH_CLOSED;
}

void ls_ssh_session_info(ls_ssh_session_t *s, ls_ssh_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s->info;
    portEXIT_CRITICAL(&s_mux);
}

const char *ls_ssh_session_server_key(const ls_ssh_session_t *s)
{
    return s->info.state == LS_SSH_CONNECTING ? NULL : s->server_key;
}

void ls_ssh_session_trust(ls_ssh_session_t *s, bool accept) { s->trust = accept ? 1 : -1; }

size_t ls_ssh_session_read(ls_ssh_session_t *s, void *buf, size_t cap)
{
    return xStreamBufferReceive(s->out, buf, cap, 0);
}

size_t ls_ssh_session_write(ls_ssh_session_t *s, const void *bytes, size_t n)
{
    if (s->info.state != LS_SSH_OPEN || !n) return 0;
    return xStreamBufferSend(s->in, bytes, n, 0);
}

void ls_ssh_session_resize(ls_ssh_session_t *s, int cols, int rows)
{
    if (cols < 2 || rows < 2 || (cols == s->cols && rows == s->rows)) return;
    s->cols = cols;
    s->rows = rows;
    s->resize_req = true;
}

void ls_ssh_session_close(ls_ssh_session_t *s) { if (s) s->close_req = true; }

void ls_ssh_session_free(ls_ssh_session_t *s)
{
    if (!s) return;
    s->close_req = true;
    /* A shell exits within one poll; give it that long before handing the
       session to the worker to free. */
    for (int i = 0; i < 10 && ls_ssh_session_state(s) != LS_SSH_CLOSED; ++i)
        vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
    bool mine;
    portENTER_CRITICAL(&s_mux);
    mine = s->done;
    if (!mine) s->detached = true;
    portEXIT_CRITICAL(&s_mux);
    if (!mine) return;                   /* the worker frees it on its way out */
    vTaskDeleteWithCaps(s->task);
    destroy(s);
}

/* ------------------------------------------------------ key operations --- */

typedef struct {
    bool keygen;
    const char *priv_in;          /* pubkey: the stored identity */
    char *priv_out;               /* keygen */
    char pub[160];
    char err[96];
    bool ok;
    bool abandoned;               /* the worker still has it: do not free */
    SemaphoreHandle_t done;
} key_job_t;

static void key_task(void *arg)
{
    key_job_t *j = arg;
    ssh_key key = NULL;
    char *priv = NULL, *pub = NULL;
    char comment[32];
    board_comment(comment, sizeof(comment));
    if (!ensure_init()) {
        snprintf(j->err, sizeof(j->err), "libssh init failed");
    } else if (j->keygen) {
        if (ssh_pki_generate_key(SSH_KEYTYPE_ED25519, NULL, &key) != SSH_OK || !key) {
            snprintf(j->err, sizeof(j->err), "key generation failed");
        } else if (ssh_pki_export_privkey_base64(key, NULL, NULL, NULL, &priv) != SSH_OK || !priv ||
                   ssh_pki_export_pubkey_base64(key, &pub) != SSH_OK || !pub) {
            snprintf(j->err, sizeof(j->err), "key export failed");
        } else {
            snprintf(j->pub, sizeof(j->pub), "ssh-ed25519 %s %s", pub, comment);
            j->priv_out = strdup(priv);
            j->ok = j->priv_out != NULL;
            if (!j->ok) snprintf(j->err, sizeof(j->err), "no memory");
        }
    } else {
        if (ssh_pki_import_privkey_base64(j->priv_in, NULL, NULL, NULL, &key) != SSH_OK || !key) {
            snprintf(j->err, sizeof(j->err), "stored identity is unreadable - 'ssh keygen force' makes a new one");
        } else if (ssh_pki_export_pubkey_base64(key, &pub) == SSH_OK && pub) {
            snprintf(j->pub, sizeof(j->pub), "ssh-ed25519 %s %s", pub, comment);
            j->ok = true;
        }
    }
    if (priv) { ls_ssh_wipe(priv, strlen(priv)); ssh_string_free_char(priv); }
    if (pub) ssh_string_free_char(pub);
    if (key) ssh_key_free(key);
    xSemaphoreGive(j->done);
    for (;;) vTaskSuspend(NULL);         /* the caller deletes this task */
}

static bool run_key_job(key_job_t *j)
{
    j->done = xSemaphoreCreateBinary();
    if (!j->done) { snprintf(j->err, sizeof(j->err), "no memory"); return false; }
    TaskHandle_t t = NULL;
    if (!start_worker(key_task, j, &t)) {
        vSemaphoreDelete(j->done);
        snprintf(j->err, sizeof(j->err), "could not start the ssh task (%u bytes of stack)", WORKER_STACK);
        return false;
    }
    if (xSemaphoreTake(j->done, pdMS_TO_TICKS(JOB_WAIT_MS)) != pdTRUE) {
        /* Still inside libssh: leave it be rather than pull its stack away.
           The job is leaked with it, deliberately. */
        ESP_LOGW(TAG, "key job still running after %d s; not waiting", JOB_WAIT_MS / 1000);
        snprintf(j->err, sizeof(j->err), "timed out");
        j->abandoned = true;
        return false;
    }
    vTaskDeleteWithCaps(t);
    vSemaphoreDelete(j->done);
    return true;
}

bool ls_ssh_keygen(char **priv_b64, char *pub_line, size_t pub_n, char *err, size_t err_n)
{
    *priv_b64 = NULL;
    key_job_t *j = calloc(1, sizeof(*j));
    if (!j) { if (err) snprintf(err, err_n, "no memory"); return false; }
    j->keygen = true;
    if (!run_key_job(j)) {
        if (err) snprintf(err, err_n, "%s", j->err);
        if (!j->abandoned) free(j);
        return false;
    }
    bool ok = j->ok;
    if (ok) { *priv_b64 = j->priv_out; snprintf(pub_line, pub_n, "%s", j->pub); }
    else if (err) snprintf(err, err_n, "%s", j->err);
    free(j);
    return ok;
}

bool ls_ssh_pubkey_line(const char *priv_b64, char *pub_line, size_t pub_n)
{
    key_job_t *j = calloc(1, sizeof(*j));
    if (!j) return false;
    j->priv_in = priv_b64;
    if (!run_key_job(j)) { if (!j->abandoned) free(j); return false; }
    bool ok = j->ok;
    if (ok) snprintf(pub_line, pub_n, "%s", j->pub);
    free(j);
    return ok;
}
