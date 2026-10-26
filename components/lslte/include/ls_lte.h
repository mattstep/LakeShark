/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. */
#pragma once

/* USB LTE stick (Huawei MS2372h and kin): mode switch, AT control on the
   PCUI port, and the CDC-NCM data path as esp_netif "LTE". See
   docs/LTE_USB.md. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ls_lte_at.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_LTE_APN_MAX 64

typedef struct {
    char apn[LS_LTE_APN_MAX]; /* empty: the APN of the stick's profile 1 */
    bool auto_dial;
} ls_lte_config_t;

typedef enum {
    LS_LTE_NONE = 0,      /* no stick */
    LS_LTE_SWITCHING,     /* mass-storage mode, switch sent */
    LS_LTE_PROBING,       /* waiting for the AT port to answer */
    LS_LTE_INIT,
    LS_LTE_SIM_ERROR,
    LS_LTE_SEARCHING,     /* not registered yet */
    LS_LTE_NO_SERVICE,    /* not registered after 90 s; still trying */
    LS_LTE_REGISTERED,    /* idle: no data session */
    LS_LTE_DIALING,
    LS_LTE_CONNECTED,     /* data session up */
    LS_LTE_HUNGUP,        /* session dropped; redial pending */
    LS_LTE_ERROR,         /* the stick does not answer */
} ls_lte_state_t;

const char *ls_lte_state_name(ls_lte_state_t s);

typedef struct {
    ls_lte_state_t state;
    uint16_t vid, pid;
    char model[32], revision[32], imei[20];
    char oper[32];
    int act;              /* +COPS AcT, 7 = LTE */
    char tech[12];        /* ^SYSINFOEX / ^HCSQ system mode */
    ls_at_signal_t sig;
    char apn[LS_LTE_APN_MAX];
    bool auto_dial;
    bool have_ip, static_ip;
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns[2]; /* lwIP order */
    uint32_t ntb_in, ntb_out;      /* negotiated NTB sizes */
    uint32_t rx_frames, tx_frames, rx_bytes, tx_bytes;
    uint32_t tx_drops;    /* not sent: no session, or the NTB did not fit */
    uint32_t tx_busy;     /* not sent: every OUT transfer in flight */
    uint32_t tx_errors;   /* sent, and the transfer failed */
    uint32_t rx_drops, rx_bad_ntb, rx_ntbs;
    int64_t session_us;   /* esp_timer time the session came up, 0 when down */
    char last_error[48];
} ls_lte_status_t;

/* After the USB host is installed. Starts the "lte" task. */
esp_err_t ls_lte_start(const ls_lte_config_t *cfg);
/* A stick is on the port (any mode): the SDR's "nothing enumerated"
   advice does not apply. */
bool ls_lte_has_device(void);
void ls_lte_get_status(ls_lte_status_t *out);

/* One AT command through the driver's channel, from another task. Blocks
   up to timeout_ms. resp gets the reply lines. */
esp_err_t ls_lte_at(const char *cmd, char *resp, size_t cap, uint32_t timeout_ms,
                    ls_at_result_t *res);

void ls_lte_dial(void);
void ls_lte_hangup(void);
void ls_lte_force_switch(void);
/* Running copy only; storing is the console's job. */
void ls_lte_set_apn(const char *apn);
void ls_lte_set_auto(bool on);

/* NVS namespace "lslte". Boot task or console only. */
void ls_lte_settings_load(ls_lte_config_t *cfg);
esp_err_t ls_lte_settings_save_apn(const char *apn);
esp_err_t ls_lte_settings_save_auto(bool on);

void ls_lte_console_register(void);

#ifdef __cplusplus
}
#endif
