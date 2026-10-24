/* See ls_ssh_store.h. Internal-RAM-stack tasks only. */

#include "ls_ssh_store.h"
#include "ls_ssh_session.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"

#define NVS_NS            "lsssh"
#define NVS_KEY_IDENTITY  "id_ed25519"

/* NVS key for a host: 15 characters at most, so a hash of "host:port". */
static void host_nvs_key(char kind, const char *host, unsigned port, char *out, size_t cap)
{
    uint32_t h = 2166136261u;
    for (const char *p = host; *p; ++p) h = (h ^ (uint8_t)tolower((unsigned char)*p)) * 16777619u;
    char ps[16];
    snprintf(ps, sizeof(ps), ":%u", port);
    for (const char *p = ps; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
    snprintf(out, cap, "%c%08lx", kind, (unsigned long)h);
}

static char *nvs_read_str(const char *key)
{
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READONLY, &nh) != ESP_OK) return NULL;
    size_t n = 0;
    char *s = NULL;
    if (nvs_get_str(nh, key, NULL, &n) == ESP_OK && n > 0) {
        s = malloc(n);
        if (s && nvs_get_str(nh, key, s, &n) != ESP_OK) { free(s); s = NULL; }
    }
    nvs_close(nh);
    return s;
}

static esp_err_t nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t nh;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nh);
    if (err != ESP_OK) return err;
    err = value ? nvs_set_str(nh, key, value) : nvs_erase_key(nh, key);
    if (err == ESP_OK) err = nvs_commit(nh);
    nvs_close(nh);
    return err;
}

char *ls_ssh_store_identity(void) { return nvs_read_str(NVS_KEY_IDENTITY); }

esp_err_t ls_ssh_store_set_identity(const char *b64) { return nvs_write_str(NVS_KEY_IDENTITY, b64); }

bool ls_ssh_store_has_identity(void)
{
    char *s = ls_ssh_store_identity();
    ls_ssh_free_secret(s);
    return s != NULL;
}

char *ls_ssh_store_host_record(const char *host, unsigned port)
{
    char key[16];
    host_nvs_key('h', host, port, key, sizeof(key));
    return nvs_read_str(key);
}

const char *ls_ssh_store_record_key(const char *rec)
{
    const char *sp = rec ? strchr(rec, ' ') : NULL;
    return sp ? sp + 1 : NULL;
}

esp_err_t ls_ssh_store_pin(const char *host, unsigned port, const char *keyline)
{
    size_t n = strlen(host) + 16 + strlen(keyline);
    char *rec = malloc(n);
    if (!rec) return ESP_ERR_NO_MEM;
    snprintf(rec, n, "%s:%u %s", host, port, keyline);
    char key[16];
    host_nvs_key('h', host, port, key, sizeof(key));
    esp_err_t err = nvs_write_str(key, rec);
    free(rec);
    return err;
}

esp_err_t ls_ssh_store_forget(const char *host, unsigned port)
{
    char key[16];
    host_nvs_key('h', host, port, key, sizeof(key));
    char *had = nvs_read_str(key);
    if (!had) return ESP_ERR_NOT_FOUND;
    free(had);
    esp_err_t err = nvs_write_str(key, NULL);
    host_nvs_key('u', host, port, key, sizeof(key));
    char *user = nvs_read_str(key);
    if (user) { free(user); nvs_write_str(key, NULL); }
    return err;
}

bool ls_ssh_store_user(const char *host, unsigned port, char *out, size_t n)
{
    char key[16];
    host_nvs_key('u', host, port, key, sizeof(key));
    char *u = nvs_read_str(key);
    if (!u) return false;
    snprintf(out, n, "%s", u);
    free(u);
    return true;
}

esp_err_t ls_ssh_store_set_user(const char *host, unsigned port, const char *user)
{
    char key[16];
    host_nvs_key('u', host, port, key, sizeof(key));
    char *had = nvs_read_str(key);
    const bool same = had && !strcmp(had, user);
    free(had);
    return same ? ESP_OK : nvs_write_str(key, user);    /* spare the flash */
}

int ls_ssh_store_each_record(void (*fn)(const char *rec, void *ctx), void *ctx)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", NVS_NS, NVS_TYPE_STR, &it);
    int n = 0;
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (info.key[0] == 'h') {
            char *v = nvs_read_str(info.key);
            if (v) { fn(v, ctx); free(v); ++n; }
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return n;
}

typedef struct { ls_ssh_host_t *out; int cap, n; } hosts_ctx_t;

static void collect(const char *rec, void *arg)
{
    hosts_ctx_t *c = arg;
    if (c->n >= c->cap) return;
    /* "host:port <key>": the port follows the last colon before the space. */
    const char *sp = strchr(rec, ' ');
    size_t len = sp ? (size_t)(sp - rec) : strlen(rec);
    const char *colon = NULL;
    for (const char *p = rec; p < rec + len; ++p) if (*p == ':') colon = p;
    if (!colon) return;
    ls_ssh_host_t *h = &c->out[c->n];
    snprintf(h->host, sizeof(h->host), "%.*s", (int)(colon - rec), rec);
    h->port = (unsigned)atoi(colon + 1);
    h->user[0] = 0;
    ls_ssh_store_user(h->host, h->port, h->user, sizeof(h->user));
    c->n++;
}

int ls_ssh_store_hosts(ls_ssh_host_t *out, int cap)
{
    hosts_ctx_t c = { out, cap, 0 };
    ls_ssh_store_each_record(collect, &c);
    return c.n;
}

void ls_ssh_split_hostport(const char *in, char *host, size_t cap, unsigned *port)
{
    snprintf(host, cap, "%s", in);
    char *colon = strrchr(host, ':');
    if (colon && colon[1] && strchr(host, ':') == colon) {   /* one colon: not an IPv6 literal */
        *colon = 0;
        *port = (unsigned)atoi(colon + 1);
    }
}
