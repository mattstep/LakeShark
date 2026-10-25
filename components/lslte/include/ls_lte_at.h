/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. */
#pragma once

/* AT command channel of a Huawei LTE stick, without the USB: bytes in,
   lines out, unsolicited result codes (URCs) told apart from the reply to
   the command in flight, and parsers for the replies the driver reads.
   Pure code; the bench builds it on the host. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_AT_LINE_MAX 256
#define LS_AT_RESP_MAX 1024
#define LS_AT_CMD_MAX  160

typedef enum {
    LS_AT_RES_NONE = 0,
    LS_AT_RES_OK,
    LS_AT_RES_ERROR,
    LS_AT_RES_CME,           /* +CME ERROR / +CMS ERROR, text in the reply */
    LS_AT_RES_NOT_SUPPORTED, /* Huawei's "COMMAND NOT SUPPORT" */
    LS_AT_RES_NO_CARRIER,
    LS_AT_RES_TIMEOUT,
    LS_AT_RES_IO,            /* the write never made it out */
} ls_at_result_t;

const char *ls_at_result_name(ls_at_result_t r);

typedef void (*ls_at_urc_fn)(void *ctx, const char *line);
/* resp: the reply's lines, '\n'-separated, the final result line last. */
typedef void (*ls_at_done_fn)(void *ctx, ls_at_result_t res, const char *resp);

typedef struct {
    ls_at_urc_fn urc;
    ls_at_done_fn done;
    void *ctx;
    char line[LS_AT_LINE_MAX];
    size_t line_len;
    bool line_overflow;
    bool busy;
    char cmd[LS_AT_CMD_MAX];  /* for dropping the echo */
    char prefix[32];          /* "+CSQ:" for AT+CSQ; empty for ATI */
    char resp[LS_AT_RESP_MAX];
    size_t resp_len;
} ls_at_t;

void ls_at_init(ls_at_t *a, ls_at_urc_fn urc, ls_at_done_fn done, void *ctx);
/* A command was written: what follows is its reply. */
void ls_at_begin(ls_at_t *a, const char *cmd);
/* Ends the command in flight with res (a timeout), passing what arrived. */
void ls_at_abort(ls_at_t *a, ls_at_result_t res);
void ls_at_feed(ls_at_t *a, const uint8_t *data, size_t n);

/* The final result a line is, LS_AT_RES_NONE when it is not one. */
ls_at_result_t ls_at_final(const char *line);
/* True for lines the stick sends on its own (^RSSI:, ^NDISSTAT:, ...). */
bool ls_at_is_urc(const char *line);
/* "+CSQ:" for "AT+CSQ", "^NDISSTATQRY:" for "AT^NDISSTATQRY?", "" for
   ATI or ATE0, whose replies carry no prefix. */
void ls_at_cmd_prefix(const char *cmd, char *out, size_t cap);

/* The value part of the first line of text starting with prefix, or NULL. */
const char *ls_at_find(const char *text, const char *prefix);
/* Field idx (0-based) of a comma-separated value list, quotes removed. The
   list ends at the end of the line. false when there is no such field. */
bool ls_at_field(const char *val, int idx, char *out, size_t cap);
bool ls_at_field_int(const char *val, int idx, int *out);

typedef struct {
    uint8_t ip[4], mask[4], gw[4], server[4], dns1[4], dns2[4];
    uint32_t rx_rate, tx_rate;
} ls_at_dhcp_t;
/* ^DHCP: addresses are hex words of the address bytes in little-endian
   order: 68702130 is 48.33.112.104. */
bool ls_at_parse_dhcp(const char *resp, ls_at_dhcp_t *d);

#define LS_AT_SIG_UNKNOWN (-32768)
typedef struct {
    char mode[12];      /* "LTE", "WCDMA", "GSM", "NOSERVICE" */
    int rssi_dbm;
    int rsrp_dbm;       /* LTE only */
    int sinr_db10;      /* tenths of dB, LTE only */
    int rsrq_db10;      /* tenths of dB, LTE only */
} ls_at_signal_t;
void ls_at_signal_clear(ls_at_signal_t *s);
/* ^HCSQ:"LTE",rssi,rsrp,sinr,rsrq (Huawei's 0..255 scales). */
bool ls_at_parse_hcsq(const char *line, ls_at_signal_t *s);
/* +CSQ: n,ber or ^RSSI:n (0..31, 99 unknown) to dBm. */
bool ls_at_parse_csq(const char *line, int *dbm);

/* +CGDCONT: cid,"IP","apn",...: the APN of cid. */
bool ls_at_parse_cgdcont_apn(const char *resp, int cid, char *apn, size_t cap);
/* +CEREG:/+CREG:/+CGREG: as a read reply (n,stat) or a URC (stat). */
bool ls_at_parse_reg(const char *resp, const char *prefix, int *stat);
/* ^NDISSTAT:<0|1>,... or ^NDISSTATQRY: <0|1>,...: IPv4 session state. */
bool ls_at_parse_ndisstat(const char *line, int *state);
/* +COPS: mode,format,"oper",act */
bool ls_at_parse_cops(const char *resp, char *oper, size_t cap, int *act);
/* ^SYSINFOEX:srv_status,srv_domain,roam,sim,lock,sysmode,"name",... */
bool ls_at_parse_sysinfoex(const char *resp, int *srv_status, char *mode, size_t cap);
/* ATI: Model:, Revision:, IMEI: lines. Missing ones stay empty. */
void ls_at_parse_ati(const char *resp, char *model, size_t mcap, char *rev, size_t rcap,
                     char *imei, size_t icap);

#ifdef __cplusplus
}
#endif
