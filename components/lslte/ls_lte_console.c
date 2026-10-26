/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. */

/* 'lte' console command and the stick's stored settings (NVS namespace
   "lslte": apn, auto). NVS is touched only from here and the boot task,
   never from the lte task. */

#include "ls_lte.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "ls_nvs_safe.h"

#define NVS_NS "lslte"

typedef struct { ls_lte_config_t *cfg; } load_ctx_t;

static esp_err_t load_job(void *ctx)
{
    ls_lte_config_t *c = ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NOT_FOUND;
    size_t n = sizeof(c->apn);
    if (nvs_get_str(h, "apn", c->apn, &n) != ESP_OK) c->apn[0] = 0;
    uint8_t a;
    if (nvs_get_u8(h, "auto", &a) == ESP_OK) c->auto_dial = a != 0;
    nvs_close(h);
    return ESP_OK;
}

void ls_lte_settings_load(ls_lte_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->auto_dial = true;
    ls_nvs_run(load_job, cfg, 0);
}

typedef struct { const char *apn; int automatic; } save_ctx_t;

static esp_err_t save_job(void *ctx)
{
    const save_ctx_t *c = ctx;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    if (c->apn) e = c->apn[0] ? nvs_set_str(h, "apn", c->apn) : nvs_erase_key(h, "apn");
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK && c->automatic >= 0) e = nvs_set_u8(h, "auto", (uint8_t)c->automatic);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t ls_lte_settings_save_apn(const char *apn)
{
    save_ctx_t c = { apn, -1 };
    return ls_nvs_run(save_job, &c, 0);
}

esp_err_t ls_lte_settings_save_auto(bool on)
{
    save_ctx_t c = { NULL, on };
    return ls_nvs_run(save_job, &c, 0);
}

#define Q(a) (unsigned)((a) & 0xff), (unsigned)((a) >> 8 & 0xff), (unsigned)((a) >> 16 & 0xff), (unsigned)((a) >> 24)

static void sig_line(const ls_at_signal_t *g)
{
    printf("signal:   ");
    if (g->rssi_dbm != LS_AT_SIG_UNKNOWN) printf("rssi %d dBm", g->rssi_dbm);
    else printf("rssi ?");
    if (g->rsrp_dbm != LS_AT_SIG_UNKNOWN) printf(", rsrp %d dBm", g->rsrp_dbm);
    if (g->sinr_db10 != LS_AT_SIG_UNKNOWN)
        printf(", sinr %s%d.%d dB", g->sinr_db10 < 0 ? "-" : "", abs(g->sinr_db10) / 10,
               abs(g->sinr_db10) % 10);
    if (g->rsrq_db10 != LS_AT_SIG_UNKNOWN)
        printf(", rsrq %s%d.%d dB", g->rsrq_db10 < 0 ? "-" : "", abs(g->rsrq_db10) / 10,
               abs(g->rsrq_db10) % 10);
    printf("\n");
}

static void status(void)
{
    ls_lte_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_SPIRAM);
    if (!st) { printf("out of memory\n"); return; }
    ls_lte_get_status(st);
    printf("state:    %s%s%s\n", ls_lte_state_name(st->state), st->last_error[0] ? " - " : "",
           st->last_error);
    if (st->vid)
        printf("device:   %04x:%04x %s rev %s IMEI %s\n", st->vid, st->pid, st->model,
               st->revision, st->imei);
    printf("operator: %s%s%s (AcT %d)\n", st->oper[0] ? st->oper : "?", st->tech[0] ? ", " : "",
           st->tech, st->act);
    sig_line(&st->sig);
    printf("apn:      %s (auto-dial %s)\n", st->apn[0] ? st->apn : "(profile 1)",
           st->auto_dial ? "on" : "off");
    printf("mac:      %02x:%02x:%02x:%02x:%02x:%02x  NTB in %lu out %lu\n", st->mac[0],
           st->mac[1], st->mac[2], st->mac[3], st->mac[4], st->mac[5],
           (unsigned long)st->ntb_in, (unsigned long)st->ntb_out);
    if (st->have_ip)
        printf("ip:       %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u %u.%u.%u.%u%s\n",
               Q(st->ip), Q(st->mask), Q(st->gw), Q(st->dns[0]), Q(st->dns[1]),
               st->static_ip ? " (static from ^DHCP)" : "");
    else
        printf("ip:       none\n");
    if (st->session_us)
        printf("session:  up %lld s\n", (long long)((esp_timer_get_time() - st->session_us) / 1000000));
    printf("rx:       %lu frames %lu bytes in %lu NTBs, %lu malformed NTBs, %lu dropped\n",
           (unsigned long)st->rx_frames, (unsigned long)st->rx_bytes, (unsigned long)st->rx_ntbs,
           (unsigned long)st->rx_bad_ntb, (unsigned long)st->rx_drops);
    printf("tx:       %lu frames %lu bytes, %lu dropped, %lu with all transfers busy, %lu "
           "failed\n", (unsigned long)st->tx_frames, (unsigned long)st->tx_bytes,
           (unsigned long)st->tx_drops, (unsigned long)st->tx_busy, (unsigned long)st->tx_errors);
    free(st);
}

static int cmd_lte(int argc, char **argv)
{
    if (argc == 1 || !strcmp(argv[1], "status")) { status(); return 0; }
    const char *op = argv[1];
    if (!strcmp(op, "at") && argc >= 3) {
        char cmd[LS_AT_CMD_MAX] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) strlcat(cmd, " ", sizeof(cmd));
            strlcat(cmd, argv[i], sizeof(cmd));
        }
        /* The console eats double quotes; a single quote stands in. */
        for (char *q = cmd; *q; q++) if (*q == '\'') *q = '"';
        char *resp = heap_caps_malloc(LS_AT_RESP_MAX, MALLOC_CAP_SPIRAM);
        if (!resp) { printf("out of memory\n"); return 1; }
        ls_at_result_t res = LS_AT_RES_NONE;
        esp_err_t e = ls_lte_at(cmd, resp, LS_AT_RESP_MAX, 5000, &res);
        if (e == ESP_OK) printf("%s[%s]\n", resp, ls_at_result_name(res));
        else printf("%s\n", e == ESP_ERR_NOT_FOUND ? "no stick attached" : esp_err_to_name(e));
        free(resp);
        return e == ESP_OK && res == LS_AT_RES_OK ? 0 : 1;
    }
    if (!strcmp(op, "dial") && argc == 2) { ls_lte_dial(); printf("dialing\n"); return 0; }
    if (!strcmp(op, "hangup") && argc == 2) { ls_lte_hangup(); printf("hanging up (auto-dial held until 'lte dial')\n"); return 0; }
    if (!strcmp(op, "switch") && argc == 2) { ls_lte_force_switch(); printf("looking for a stick in mass-storage mode\n"); return 0; }
    if (!strcmp(op, "apn") && (argc == 2 || argc == 3)) {
        const char *apn = argc == 3 ? argv[2] : "";
        if (!strcmp(apn, "-")) apn = "";
        if (strlen(apn) >= LS_LTE_APN_MAX) { printf("too long\n"); return 1; }
        esp_err_t e = ls_lte_settings_save_apn(apn);
        ls_lte_set_apn(apn);
        printf("apn %s%s (next dial)\n", apn[0] ? apn : "from profile 1",
               e == ESP_OK ? "" : " - not stored");
        return 0;
    }
    if (!strcmp(op, "auto") && argc == 3 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        const bool on = !strcmp(argv[2], "on");
        esp_err_t e = ls_lte_settings_save_auto(on);
        ls_lte_set_auto(on);
        printf("auto-dial %s%s\n", on ? "on" : "off", e == ESP_OK ? "" : " - not stored");
        return 0;
    }
    printf("usage: lte [status | at <cmd> | dial | hangup | apn [<apn>|-] | auto on|off | switch]\n");
    return 1;
}

void ls_lte_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "lte",
        .help = "USB LTE stick: state, signal, address, counters. 'lte at <cmd>' (write ' for a "
                "double quote), 'lte dial', 'lte hangup', 'lte apn <apn>|-' and 'lte auto on|off' "
                "(stored), 'lte switch' sends the mode switch to a stick in storage mode",
        .hint = "[status|at <cmd>|dial|hangup|apn [<apn>|-]|auto on|off|switch]",
        .func = &cmd_lte,
    };
    esp_console_cmd_register(&cmd);
}
