/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. */

#include "ls_lte_at.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *ls_at_result_name(ls_at_result_t r)
{
    switch (r) {
    case LS_AT_RES_NONE:          return "none";
    case LS_AT_RES_OK:            return "OK";
    case LS_AT_RES_ERROR:         return "ERROR";
    case LS_AT_RES_CME:           return "CME ERROR";
    case LS_AT_RES_NOT_SUPPORTED: return "NOT SUPPORTED";
    case LS_AT_RES_NO_CARRIER:    return "NO CARRIER";
    case LS_AT_RES_TIMEOUT:       return "timeout";
    case LS_AT_RES_IO:            return "write failed";
    }
    return "?";
}

void ls_at_init(ls_at_t *a, ls_at_urc_fn urc, ls_at_done_fn done, void *ctx)
{
    memset(a, 0, sizeof(*a));
    a->urc = urc;
    a->done = done;
    a->ctx = ctx;
}

void ls_at_cmd_prefix(const char *cmd, char *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    if ((cmd[0] != 'A' && cmd[0] != 'a') || (cmd[1] != 'T' && cmd[1] != 't')) return;
    const char *s = cmd + 2;
    if (*s != '+' && *s != '^') return;
    size_t n = strcspn(s, "?=;\r\n");
    if (n + 2 > cap) return;
    memcpy(out, s, n);
    out[n] = ':';
    out[n + 1] = 0;
}

void ls_at_begin(ls_at_t *a, const char *cmd)
{
    a->busy = true;
    a->resp_len = 0;
    a->resp[0] = 0;
    snprintf(a->cmd, sizeof(a->cmd), "%s", cmd);
    ls_at_cmd_prefix(cmd, a->prefix, sizeof(a->prefix));
}

static void finish(ls_at_t *a, ls_at_result_t res)
{
    a->busy = false;
    if (a->done) a->done(a->ctx, res, a->resp);
}

void ls_at_abort(ls_at_t *a, ls_at_result_t res)
{
    if (a->busy) finish(a, res);
}

ls_at_result_t ls_at_final(const char *line)
{
    if (!strcmp(line, "OK")) return LS_AT_RES_OK;
    if (!strcmp(line, "ERROR")) return LS_AT_RES_ERROR;
    if (!strcmp(line, "COMMAND NOT SUPPORT")) return LS_AT_RES_NOT_SUPPORTED;
    if (!strcmp(line, "NO CARRIER")) return LS_AT_RES_NO_CARRIER;
    if (!strncmp(line, "+CME ERROR", 10) || !strncmp(line, "+CMS ERROR", 10)) return LS_AT_RES_CME;
    return LS_AT_RES_NONE;
}

bool ls_at_is_urc(const char *line)
{
    /* What the MS2372h sends unasked: signal, session, SIM toolkit, mode
       and service changes, flow reports, and registration when enabled. */
    static const char *const urc[] = {
        "^RSSI:", "^HCSQ:", "^NDISSTAT:", "^STIN:", "^BOOT:", "^MODE:", "^SRVST:",
        "^SIMST:", "^DSFLOWRPT:", "^RSSILVL:", "^CERSSI:", "^LTERSRP:", "^EONS:",
        "^ACTIVEBAND:", "^SYSSTART", "^NWTIME:", "^TIME:", "+CEREG:", "+CREG:",
        "+CGREG:", "+CGEV:", "^PLMN:", "^CSNR:", "^ECCLIST:", "^CPIN:", "^ANLEVEL:",
    };
    for (size_t i = 0; i < sizeof(urc) / sizeof(urc[0]); i++)
        if (!strncmp(line, urc[i], strlen(urc[i]))) return true;
    return false;
}

static void append(ls_at_t *a, const char *line)
{
    const size_t n = strlen(line);
    if (a->resp_len + n + 2 > sizeof(a->resp)) return; /* keeps the head */
    memcpy(a->resp + a->resp_len, line, n);
    a->resp_len += n;
    a->resp[a->resp_len++] = '\n';
    a->resp[a->resp_len] = 0;
}

static void on_line(ls_at_t *a, const char *line)
{
    if (a->busy && !strcmp(line, a->cmd)) return; /* echo, before ATE0 */
    const ls_at_result_t f = a->busy ? ls_at_final(line) : LS_AT_RES_NONE;
    if (f != LS_AT_RES_NONE) {
        append(a, line);
        finish(a, f);
        return;
    }
    if (ls_at_is_urc(line)) {
        /* A URC that is also the reply's prefix (AT^HCSQ? answers ^HCSQ:)
           is both. */
        if (a->busy && a->prefix[0] && !strncmp(line, a->prefix, strlen(a->prefix)))
            append(a, line);
        if (a->urc) a->urc(a->ctx, line);
        return;
    }
    if (a->busy) append(a, line);
    else if (a->urc) a->urc(a->ctx, line);
}

void ls_at_feed(ls_at_t *a, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char c = (char)data[i];
        if (c == '\r' || c == '\n') {
            if (a->line_len && !a->line_overflow) {
                a->line[a->line_len] = 0;
                on_line(a, a->line);
            }
            a->line_len = 0;
            a->line_overflow = false;
            continue;
        }
        if (c == 0) continue;
        if (a->line_len + 1 >= sizeof(a->line)) { a->line_overflow = true; continue; }
        a->line[a->line_len++] = c;
    }
}

const char *ls_at_find(const char *text, const char *prefix)
{
    const size_t n = strlen(prefix);
    for (const char *l = text; l && *l; ) {
        if (!strncmp(l, prefix, n)) {
            l += n;
            while (*l == ' ') l++;
            return l;
        }
        l = strchr(l, '\n');
        if (l) l++;
    }
    return NULL;
}

bool ls_at_field(const char *val, int idx, char *out, size_t cap)
{
    if (!val) return false;
    const char *s = val;
    for (int k = 0; k < idx; k++) {
        bool q = false;
        while (*s && *s != '\n' && *s != '\r' && (q || *s != ',')) {
            if (*s == '"') q = !q;
            s++;
        }
        if (*s != ',') return false;
        s++;
    }
    while (*s == ' ') s++;
    size_t o = 0;
    bool q = false;
    for (; *s && *s != '\n' && *s != '\r' && (q || *s != ','); s++) {
        if (*s == '"') { q = !q; continue; }
        if (o + 1 < cap) out[o++] = *s;
    }
    if (cap) out[o] = 0;
    return true;
}

bool ls_at_field_int(const char *val, int idx, int *out)
{
    char f[16];
    if (!ls_at_field(val, idx, f, sizeof(f)) || !f[0]) return false;
    char *end;
    long v = strtol(f, &end, 10);
    if (*end) return false;
    *out = (int)v;
    return true;
}

static bool hex_addr(const char *val, int idx, uint8_t out[4])
{
    char f[16];
    if (!ls_at_field(val, idx, f, sizeof(f)) || strlen(f) != 8) return false;
    char *end;
    unsigned long v = strtoul(f, &end, 16);
    if (*end) return false;
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(v >> (8 * i));
    return true;
}

bool ls_at_parse_dhcp(const char *resp, ls_at_dhcp_t *d)
{
    const char *v = ls_at_find(resp, "^DHCP:");
    if (!v) return false;
    memset(d, 0, sizeof(*d));
    if (!hex_addr(v, 0, d->ip) || !hex_addr(v, 1, d->mask) || !hex_addr(v, 2, d->gw)) return false;
    hex_addr(v, 3, d->server);
    hex_addr(v, 4, d->dns1);
    hex_addr(v, 5, d->dns2);
    char f[16];
    if (ls_at_field(v, 6, f, sizeof(f))) d->rx_rate = (uint32_t)strtoul(f, NULL, 10);
    if (ls_at_field(v, 7, f, sizeof(f))) d->tx_rate = (uint32_t)strtoul(f, NULL, 10);
    return true;
}

void ls_at_signal_clear(ls_at_signal_t *s)
{
    memset(s, 0, sizeof(*s));
    s->rssi_dbm = s->rsrp_dbm = s->sinr_db10 = s->rsrq_db10 = LS_AT_SIG_UNKNOWN;
}

bool ls_at_parse_hcsq(const char *line, ls_at_signal_t *s)
{
    const char *v = ls_at_find(line, "^HCSQ:");
    if (!v) return false;
    ls_at_signal_t n;
    ls_at_signal_clear(&n);
    if (!ls_at_field(v, 0, n.mode, sizeof(n.mode))) return false;
    int raw[4];
    bool have[4];
    for (int i = 0; i < 4; i++) have[i] = ls_at_field_int(v, i + 1, &raw[i]) && raw[i] != 255;
    /* Huawei's scales: 1 is the bottom 1 dB (0.2 dB, 0.5 dB) step,
       0 anything below it. RSSI and RSCP share the RSSI scale. */
    if (have[0] && raw[0] > 0) n.rssi_dbm = raw[0] - 121;
    if (!strcmp(n.mode, "LTE")) {
        if (have[1] && raw[1] > 0) n.rsrp_dbm = raw[1] - 141;
        if (have[2] && raw[2] > 0) n.sinr_db10 = -200 + (raw[2] - 1) * 2;
        if (have[3] && raw[3] > 0) n.rsrq_db10 = -195 + (raw[3] - 1) * 5;
    }
    *s = n;
    return true;
}

bool ls_at_parse_csq(const char *line, int *dbm)
{
    const char *v = ls_at_find(line, "+CSQ:");
    if (!v) v = ls_at_find(line, "^RSSI:");
    int n;
    if (!v || !ls_at_field_int(v, 0, &n)) return false;
    *dbm = n >= 0 && n <= 31 ? -113 + 2 * n : LS_AT_SIG_UNKNOWN;
    return true;
}

bool ls_at_parse_cgdcont_apn(const char *resp, int cid, char *apn, size_t cap)
{
    for (const char *l = resp; l && *l; ) {
        const char *v = ls_at_find(l, "+CGDCONT:");
        if (!v) return false;
        int c;
        if (ls_at_field_int(v, 0, &c) && c == cid) return ls_at_field(v, 2, apn, cap);
        l = strchr(v, '\n');
        if (l) l++;
    }
    return false;
}

bool ls_at_parse_reg(const char *resp, const char *prefix, int *stat)
{
    const char *v = ls_at_find(resp, prefix);
    int a, b;
    if (!v || !ls_at_field_int(v, 0, &a)) return false;
    /* The read reply leads with the URC mode (n,stat,...); a URC carries
       the state alone, then in mode 2 quoted location fields. */
    const char *c = strchr(v, ',');
    const char *eol = v + strcspn(v, "\r\n");
    if (c && c < eol && c[1] != '"' && ls_at_field_int(v, 1, &b)) *stat = b;
    else *stat = a;
    return true;
}

bool ls_at_parse_ndisstat(const char *line, int *state)
{
    const char *v = ls_at_find(line, "^NDISSTATQRY:");
    if (!v) v = ls_at_find(line, "^NDISSTAT:");
    return v && ls_at_field_int(v, 0, state);
}

bool ls_at_parse_cops(const char *resp, char *oper, size_t cap, int *act)
{
    const char *v = ls_at_find(resp, "+COPS:");
    if (!v) return false;
    if (cap) oper[0] = 0;
    *act = -1;
    ls_at_field(v, 2, oper, cap);
    ls_at_field_int(v, 3, act);
    return true;
}

bool ls_at_parse_sysinfoex(const char *resp, int *srv_status, char *mode, size_t cap)
{
    const char *v = ls_at_find(resp, "^SYSINFOEX:");
    if (!v || !ls_at_field_int(v, 0, srv_status)) return false;
    if (cap) mode[0] = 0;
    ls_at_field(v, 6, mode, cap);
    return true;
}

static void ati_field(const char *resp, const char *key, char *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    const char *v = ls_at_find(resp, key);
    if (!v) return;
    size_t n = strcspn(v, "\r\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n);
    out[n] = 0;
}

void ls_at_parse_ati(const char *resp, char *model, size_t mcap, char *rev, size_t rcap,
                     char *imei, size_t icap)
{
    ati_field(resp, "Model:", model, mcap);
    ati_field(resp, "Revision:", rev, rcap);
    ati_field(resp, "IMEI:", imei, icap);
}
