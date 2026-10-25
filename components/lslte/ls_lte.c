/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. The NCM side follows the USB CDC-NCM 1.0
   specification; the order of the setup requests is the one Linux's
   cdc_ncm/huawei_cdc_ncm drivers use, which these sticks are tested with. */

/* One USB host client and one task ("lte") for a Huawei LTE stick:

   - mass-storage mode (12d1:14fe and kin): send the mode-switch CBW, let
     the stick drop off and come back as a modem;
   - modem mode: the PCUI interface (ff/02/12) carries AT commands, the
     vendor-class NCM interface (ff/02/16) carries Ethernet frames in NTB16
     blocks; its alt 0 has only the notification endpoint, alt 1 adds the
     bulk pair.

   Everything USB happens on the lte task: transfer callbacks run inside its
   usb_host_client_handle_events, and the AT/dial state machine runs on the
   100 ms timeout of the same call. The only other entry points are the
   netif's transmit (tcpip task: builds an NTB, submits it), the IP event
   handler (flags only), and the console (requests through flags and a
   semaphore). */

#include "ls_lte.h"
#include "ls_ncm.h"
#include "ls_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_memory_utils.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

static const char *TAG = "lte";

#define HUAWEI_VID     0x12d1
#define CLS_VENDOR     0xff
#define SUB_HUAWEI     0x02
#define PROTO_MODEM    0x01
#define PROTO_PCUI     0x12
#define PROTO_NCM      0x16

#define NCM_GET_NTB_PARAMETERS 0x80
#define NCM_SET_NTB_FORMAT     0x84
#define NCM_SET_NTB_INPUT_SIZE 0x86
#define NCM_SET_CRC_MODE       0x8a
#define CDC_SET_PACKET_FILTER  0x43
#define CDC_NOTIFY_NETWORK_CONNECTION 0x00
#define CDC_NOTIFY_SPEED_CHANGE       0x2a

#define TICK_MS          100
#define RX_NTB_CAP       16384
#define TX_NTB_CAP       2048
/* One frame per NTB, so a burst of TCP ACKs needs several in flight;
   with two, a 1 MB download dropped 24 of 430 frames. */
#define NTX              4
#define AT_RX_SIZE       1024
#define AT_TIMEOUT_MS    5000
#define DIAL_TIMEOUT_US  (30 * 1000000LL)
#define REG_TIMEOUT_US   (90 * 1000000LL)
#define DHCP_WAIT_US     (15 * 1000000LL)
#define REDIAL_US        (10 * 1000000LL)
#define SIG_POLL_US      (30 * 1000000LL)
#define SEEN_HOLD_US     (30 * 1000000LL)

/* usb_modeswitch's HuaweiNewMode message: a vendor SCSI command (0x11
   0x06) in a Bulk-Only CBW. */
static const uint8_t k_switch_cbw[31] = {
    0x55, 0x53, 0x42, 0x43, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a,
    0x11, 0x06, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
};

typedef enum { SP_NONE, SP_HANGUP, SP_CSQ, SP_DHCP, SP_NDISQ } special_t;
typedef enum { OWN_SELF, OWN_CONSOLE } owner_t;

typedef struct lte lte_t;

/* What esp_netif sees as the driver: its base first, as esp_netif_attach
   requires. */
typedef struct {
    esp_netif_driver_base_t base;
    lte_t *s;
} glue_t;

struct lte {
    usb_host_client_handle_t client;
    SemaphoreHandle_t tx_mutex, con_mutex, con_sem;

    /* From the client event callback, read by the task loop. */
    uint8_t new_addr[4];
    int new_n;
    bool gone;
    bool scan_existing;

    /* The attached stick. */
    usb_device_handle_t dev;
    uint8_t addr;
    bool at_claimed, ncm_claimed;
    uint8_t at_itf, at_in, at_out;
    uint8_t ncm_itf, ncm_in, ncm_out, ncm_int;
    uint16_t ncm_mps, ncm_int_mps;
    uint8_t mac_idx, ncm_caps;
    usb_transfer_t *ctrl, *at_rx, *at_tx, *rx[2], *tx[NTX], *intr;
    volatile bool ctrl_done;
    bool at_rx_busy, at_tx_busy, rx_busy[2], tx_busy[NTX], intr_busy;
    bool tearing_down, data_ok;
    bool lost; /* a transfer came back NO_DEVICE: DEV_GONE is on its way */
    ls_ncm_params_t ntb;
    uint16_t tx_seq;
    int64_t seen_us; /* last time a Huawei device was on the port */

    /* netif */
    esp_netif_t *netif;
    glue_t glue;
    bool netif_started, netif_connected;
    volatile bool ev_got_ip, ev_lost_ip;

    /* AT engine */
    ls_at_t at;
    bool at_done;
    ls_at_result_t at_res;
    owner_t at_owner;
    int64_t at_deadline;
    ls_lte_state_t cmd_state;
    int cmd_step;
    special_t cmd_special;

    /* State machine */
    int step, tries;
    int64_t t_enter, wake, t_dial, t_conn, last_sig, last_poll;
    bool hold, dhcp_fallback_tried;
    volatile bool req_dial, req_hangup, req_switch;
    bool urc_ndis_up, urc_ndis_down;
    char profile_apn[LS_LTE_APN_MAX];
    char apn_cfg[LS_LTE_APN_MAX];
    bool auto_dial;

    /* Console AT request */
    volatile bool con_pending;
    uint32_t con_seq, con_seq_sent;
    char con_cmd[LS_AT_CMD_MAX];
    char con_resp[LS_AT_RESP_MAX];
    ls_at_result_t con_res;

    ls_lte_status_t st;
};

static lte_t *s_lte;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

const char *ls_lte_state_name(ls_lte_state_t s)
{
    switch (s) {
    case LS_LTE_NONE:       return "no device";
    case LS_LTE_SWITCHING:  return "mode switch";
    case LS_LTE_PROBING:    return "probing";
    case LS_LTE_INIT:       return "init";
    case LS_LTE_SIM_ERROR:  return "SIM error";
    case LS_LTE_SEARCHING:  return "searching";
    case LS_LTE_NO_SERVICE: return "no service";
    case LS_LTE_REGISTERED: return "registered";
    case LS_LTE_DIALING:    return "dialing";
    case LS_LTE_CONNECTED:  return "connected";
    case LS_LTE_HUNGUP:     return "hung up";
    case LS_LTE_ERROR:      return "error";
    }
    return "?";
}

static int64_t now_us(void) { return esp_timer_get_time(); }

static void set_error(lte_t *s, const char *fmt, const char *arg)
{
    snprintf(s->st.last_error, sizeof(s->st.last_error), fmt, arg ? arg : "");
}

/* ------------------------------------------------------------ USB plumbing */

static void client_cb(const usb_host_client_event_msg_t *m, void *arg)
{
    lte_t *s = arg;
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (s->new_n < (int)sizeof(s->new_addr)) s->new_addr[s->new_n++] = m->new_dev.address;
    } else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (s->dev && m->dev_gone.dev_hdl == s->dev) s->gone = true;
    }
}

/* Runs the client's events until *flag or ms pass. Only from the task
   loop's own level, never from a callback. */
static bool pump_until(lte_t *s, volatile bool *flag, int ms)
{
    const int64_t end = now_us() + (int64_t)ms * 1000;
    while (!*flag && now_us() < end) usb_host_client_handle_events(s->client, pdMS_TO_TICKS(10));
    return *flag;
}

static void ctrl_cb(usb_transfer_t *t)
{
    ((lte_t *)t->context)->ctrl_done = true;
}

/* A control request on EP0, waited for. data: wLength bytes out, or room
   for them in. */
static esp_err_t ctrl(lte_t *s, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                      void *data, uint16_t len, int *got)
{
    if (!s->ctrl || len > s->ctrl->data_buffer_size - sizeof(usb_setup_packet_t))
        return ESP_ERR_NO_MEM;
    usb_setup_packet_t *sp = (usb_setup_packet_t *)s->ctrl->data_buffer;
    sp->bmRequestType = type;
    sp->bRequest = req;
    sp->wValue = value;
    sp->wIndex = index;
    sp->wLength = len;
    if (!(type & USB_BM_REQUEST_TYPE_DIR_IN) && len) memcpy(sp + 1, data, len);
    s->ctrl->num_bytes = sizeof(usb_setup_packet_t) + len;
    s->ctrl->device_handle = s->dev;
    s->ctrl->bEndpointAddress = 0;
    s->ctrl->callback = ctrl_cb;
    s->ctrl->context = s;
    s->ctrl->timeout_ms = 1000;
    s->ctrl_done = false;
    esp_err_t e = usb_host_transfer_submit_control(s->client, s->ctrl);
    if (e != ESP_OK) return e;
    if (!pump_until(s, &s->ctrl_done, 2000)) {
        /* Never came back: leave it allocated rather than free a transfer
           the host still owns. Later requests fail with NO_MEM. */
        ESP_LOGE(TAG, "control request 0x%02x lost", req);
        s->ctrl = NULL;
        return ESP_ERR_TIMEOUT;
    }
    if (s->ctrl->status != USB_TRANSFER_STATUS_COMPLETED) return ESP_FAIL;
    const int n = s->ctrl->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
    if ((type & USB_BM_REQUEST_TYPE_DIR_IN) && data && n > 0) memcpy(data, sp + 1, n);
    if (got) *got = n;
    return ESP_OK;
}

#define REQ_OUT_STD_EP  (USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_ENDPOINT)
#define REQ_OUT_STD_ITF (USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE)
#define REQ_IN_STD_DEV  (USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE)
#define REQ_OUT_CLS_ITF (USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE)
#define REQ_IN_CLS_ITF  (USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE)

/* A newly claimed interface's pipes start at DATA0 while the stick's
   endpoints carry on from the last session's toggle; a mismatched IN
   packet is taken for a retry and silently dropped. CLEAR_FEATURE(HALT)
   resets the device's side too. */
static void ep_sync(lte_t *s, uint8_t ep)
{
    esp_err_t e = ctrl(s, REQ_OUT_STD_EP, USB_B_REQUEST_CLEAR_FEATURE, 0, ep, NULL, 0, NULL);
    if (e != ESP_OK) ESP_LOGW(TAG, "CLEAR_FEATURE(0x%02x): %s", ep, esp_err_to_name(e));
}

/* --------------------------------------------------------------- AT side */

static void at_rx_cb(usb_transfer_t *t);

static void at_rx_post(lte_t *s)
{
    if (!s->at_rx || s->at_rx_busy || s->tearing_down || s->lost || !s->at_claimed) return;
    s->at_rx->num_bytes = AT_RX_SIZE;
    s->at_rx_busy = usb_host_transfer_submit(s->at_rx) == ESP_OK;
}

static void at_rx_cb(usb_transfer_t *t)
{
    lte_t *s = t->context;
    s->at_rx_busy = false;
    if (t->status == USB_TRANSFER_STATUS_NO_DEVICE) s->lost = true;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (t->actual_num_bytes > 0) ls_at_feed(&s->at, t->data_buffer, t->actual_num_bytes);
        /* Straight back: a gap without a posted IN loses what the stick
           sends meanwhile. Errors wait for the next tick instead. */
        at_rx_post(s);
    }
}

static void at_tx_cb(usb_transfer_t *t)
{
    lte_t *s = t->context;
    s->at_tx_busy = false;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) ls_at_abort(&s->at, LS_AT_RES_IO);
}

static void at_done(void *ctx, ls_at_result_t res, const char *resp)
{
    lte_t *s = ctx;
    s->at_done = true;
    s->at_res = res;
}

static void at_urc(void *ctx, const char *line)
{
    lte_t *s = ctx;
    int v;
    if (ls_at_parse_ndisstat(line, &v)) {
        ESP_LOGW(TAG, "%s", line);
        if (v == 1) s->urc_ndis_up = true;
        else s->urc_ndis_down = true;
    } else if (!strncmp(line, "^HCSQ:", 6)) {
        ls_at_signal_t sig;
        if (ls_at_parse_hcsq(line, &sig)) {
            s->st.sig = sig;
            if (sig.mode[0]) snprintf(s->st.tech, sizeof(s->st.tech), "%s", sig.mode);
            s->last_sig = now_us();
        }
    } else if (!strncmp(line, "^RSSI:", 6)) {
        if (ls_at_parse_csq(line, &v)) { s->st.sig.rssi_dbm = v; s->last_sig = now_us(); }
    } else if (strncmp(line, "^DSFLOWRPT:", 11)) {
        ESP_LOGD(TAG, "urc: %s", line);
    }
}

/* Writes one command. false: the write could not be started. */
static bool at_send(lte_t *s, const char *cmd, uint32_t ms, owner_t owner, special_t sp)
{
    const size_t n = strlen(cmd);
    if (!s->at_tx || s->at_tx_busy || s->at.busy || s->lost ||
        n + 1 > s->at_tx->data_buffer_size)
        return false;
    memcpy(s->at_tx->data_buffer, cmd, n);
    s->at_tx->data_buffer[n] = '\r';
    s->at_tx->num_bytes = n + 1;
    ls_at_begin(&s->at, cmd);
    s->at_owner = owner;
    s->cmd_state = s->st.state;
    s->cmd_step = s->step;
    s->cmd_special = sp;
    s->at_deadline = now_us() + (int64_t)ms * 1000;
    s->at_tx_busy = usb_host_transfer_submit(s->at_tx) == ESP_OK;
    if (!s->at_tx_busy) ls_at_abort(&s->at, LS_AT_RES_IO);
    return true;
}

/* ------------------------------------------------------------- NCM side */

static void rx_deliver(void *ctx, const uint8_t *d, size_t len)
{
    lte_t *s = ctx;
    if (!s->netif_started) return;
    /* lwIP keeps the frame past this callback (CONFIG_LWIP_L2_TO_L3_COPY
       is off, so its pbuf points at our buffer) and hands it back through
       free_rx. The NTB buffer is reposted at once, so every frame gets its
       own copy, in PSRAM. */
    uint8_t *b = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) b = malloc(len);
    if (!b) { s->st.rx_drops++; return; }
    memcpy(b, d, len);
    s->st.rx_frames++;
    s->st.rx_bytes += len;
    esp_netif_receive(s->netif, b, len, NULL);
}

static void rx_post(lte_t *s, int i)
{
    if (!s->rx[i] || s->rx_busy[i] || s->tearing_down || s->lost || !s->ncm_claimed) return;
    s->rx[i]->num_bytes = s->rx[i]->data_buffer_size;
    s->rx_busy[i] = usb_host_transfer_submit(s->rx[i]) == ESP_OK;
}

static void rx_cb(usb_transfer_t *t)
{
    lte_t *s = t->context;
    const int i = t == s->rx[1];
    s->rx_busy[i] = false;
    if (t->status == USB_TRANSFER_STATUS_NO_DEVICE) s->lost = true;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) return;
    if (t->actual_num_bytes > 0) {
        ls_ncm_err_t err;
        s->st.rx_ntbs++;
        ls_ncm_parse(t->data_buffer, t->actual_num_bytes, rx_deliver, s, &err);
        if (err != LS_NCM_OK) {
            s->st.rx_bad_ntb++;
            ESP_LOGW(TAG, "bad NTB (%d bytes): %s", t->actual_num_bytes, ls_ncm_err_name(err));
        }
    }
    rx_post(s, i);
}

static void tx_cb(usb_transfer_t *t)
{
    lte_t *s = t->context;
    int i = 0;
    while (i < NTX - 1 && s->tx[i] != t) i++;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) s->st.tx_errors++;
    taskENTER_CRITICAL(&s_lock);
    s->tx_busy[i] = false;
    taskEXIT_CRITICAL(&s_lock);
}

static void intr_post(lte_t *s)
{
    if (!s->intr || s->intr_busy || s->tearing_down || s->lost || !s->ncm_claimed) return;
    s->intr->num_bytes = s->ncm_int_mps;
    s->intr_busy = usb_host_transfer_submit(s->intr) == ESP_OK;
}

static void intr_cb(usb_transfer_t *t)
{
    lte_t *s = t->context;
    s->intr_busy = false;
    if (t->status == USB_TRANSFER_STATUS_NO_DEVICE) s->lost = true;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) return;
    const uint8_t *b = t->data_buffer;
    if (t->actual_num_bytes >= 8 && b[0] == 0xa1) {
        if (b[1] == CDC_NOTIFY_NETWORK_CONNECTION)
            ESP_LOGW(TAG, "NCM: network %s", b[2] | b[3] << 8 ? "connected" : "disconnected");
        else if (b[1] == CDC_NOTIFY_SPEED_CHANGE && t->actual_num_bytes >= 16)
            ESP_LOGW(TAG, "NCM: link %lu down / %lu up bit/s",
                     (unsigned long)(b[8] | b[9] << 8 | b[10] << 16 | (uint32_t)b[11] << 24),
                     (unsigned long)(b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24));
    }
    intr_post(s);
}

/* esp_netif transmit, on the tcpip task: one frame, one NTB. */
static esp_err_t lte_transmit(void *h, void *buffer, size_t len)
{
    lte_t *s = ((glue_t *)h)->s;
    xSemaphoreTake(s->tx_mutex, portMAX_DELAY);
    esp_err_t rc = ESP_FAIL;
    int i = -1;
    if (s->data_ok && !s->lost) {
        taskENTER_CRITICAL(&s_lock);
        for (int k = 0; k < NTX && i < 0; k++)
            if (s->tx[k] && !s->tx_busy[k]) { s->tx_busy[k] = true; i = k; }
        taskEXIT_CRITICAL(&s_lock);
    }
    if (i >= 0) {
        usb_transfer_t *t = s->tx[i];
        const size_t n = ls_ncm_build(t->data_buffer, t->data_buffer_size, &s->ntb, s->ncm_mps,
                                      s->tx_seq++, buffer, len);
        if (n) {
            t->num_bytes = n;
            rc = usb_host_transfer_submit(t);
        }
        if (rc != ESP_OK) {
            taskENTER_CRITICAL(&s_lock);
            s->tx_busy[i] = false;
            taskEXIT_CRITICAL(&s_lock);
        }
    }
    xSemaphoreGive(s->tx_mutex);
    if (rc == ESP_OK) {
        s->st.tx_frames++;
        s->st.tx_bytes += len;
    } else {
        if (i < 0 && s->data_ok) s->st.tx_busy++;
        else s->st.tx_drops++;
        rc = ESP_ERR_NO_MEM;
    }
    return rc;
}

static void lte_free_rx(void *h, void *buffer)
{
    free(buffer);
}

static esp_err_t glue_post_attach(esp_netif_t *netif, void *args)
{
    glue_t *g = args;
    g->base.netif = netif;
    const esp_netif_driver_ifconfig_t ifc = {
        .handle = g,
        .transmit = lte_transmit,
        .driver_free_rx_buffer = lte_free_rx,
    };
    return esp_netif_set_driver_config(netif, &ifc);
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    lte_t *s = arg;
    const ip_event_got_ip_t *ev = data;
    if (!ev || ev->esp_netif != s->netif) return;
    if (id == IP_EVENT_ETH_GOT_IP) s->ev_got_ip = true;
    else if (id == IP_EVENT_ETH_LOST_IP) s->ev_lost_ip = true;
    if (s->client) usb_host_client_unblock(s->client);
}

static esp_err_t netif_create(lte_t *s)
{
    if (s->netif) return ESP_OK;
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    /* IPv4 only: the session is dialled "IPV4". */
    base.flags = ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_GARP | ESP_NETIF_FLAG_EVENT_IP_MODIFIED;
    base.if_key = "LTE";
    base.if_desc = "lte";
    /* Below the Wi-Fi station's 100: with both up, Wi-Fi carries the
       default route unless 'net prefer lte'. */
    base.route_prio = 50;
    const esp_netif_config_t cfg = { .base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    s->netif = esp_netif_new(&cfg);
    if (!s->netif) return ESP_ERR_NO_MEM;
    s->glue.base.post_attach = glue_post_attach;
    s->glue.s = s;
    esp_err_t e = esp_netif_attach(s->netif, &s->glue);
    if (e != ESP_OK) {
        esp_netif_destroy(s->netif);
        s->netif = NULL;
    }
    return e;
}

static void session_up(lte_t *s)
{
    if (!s->netif_started || s->netif_connected) return;
    esp_netif_dhcp_status_t st;
    /* After a static fallback DHCP is stopped, and esp_netif's connected
       action would re-announce the old static address. */
    if (esp_netif_dhcpc_get_status(s->netif, &st) == ESP_OK && st == ESP_NETIF_DHCP_STOPPED) {
        esp_netif_ip_info_t zero = {0};
        esp_netif_set_ip_info(s->netif, &zero);
        esp_netif_dhcpc_start(s->netif);
    }
    s->st.have_ip = s->st.static_ip = false;
    s->dhcp_fallback_tried = false;
    s->ev_got_ip = false;
    esp_netif_action_connected(s->netif, NULL, 0, NULL);
    s->netif_connected = true;
    s->st.session_us = now_us();
    s->t_conn = now_us();
}

static void session_down(lte_t *s, const char *why)
{
    if (s->netif_connected) {
        esp_netif_action_disconnected(s->netif, NULL, 0, NULL);
        s->netif_connected = false;
        ESP_LOGW(TAG, "data session down (%s)", why);
    }
    s->st.have_ip = false;
    s->st.ip = s->st.mask = s->st.gw = s->st.dns[0] = s->st.dns[1] = 0;
    s->st.session_us = 0;
    s->urc_ndis_up = false;
    ls_net_reevaluate();
}

/* ----------------------------------------------------- attach and detach */

static usb_transfer_t *xfer(lte_t *s, size_t size, uint8_t ep, usb_transfer_cb_t cb)
{
    usb_transfer_t *t = NULL;
    if (usb_host_transfer_alloc(size, 0, &t) != ESP_OK) return NULL;
    t->device_handle = s->dev;
    t->bEndpointAddress = ep;
    t->callback = cb;
    t->context = s;
    t->timeout_ms = 0;
    return t;
}

/* Bulk (and interrupt) endpoints of one alt setting. */
static void endpoints(const usb_config_desc_t *cd, const usb_intf_desc_t *in, int off,
                      uint8_t *bin, uint8_t *bout, uint8_t *iin, uint16_t *mps, uint16_t *imps)
{
    for (int i = 0; i < in->bNumEndpoints; i++) {
        int eo = off;
        const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(in, i, cd->wTotalLength, &eo);
        if (!ep) continue;
        const int type = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
        const bool is_in = USB_EP_DESC_GET_EP_DIR(ep);
        if (type == USB_BM_ATTRIBUTES_XFER_BULK) {
            if (is_in) *bin = ep->bEndpointAddress;
            else *bout = ep->bEndpointAddress;
            if (mps) *mps = USB_EP_DESC_GET_MPS(ep);
        } else if (type == USB_BM_ATTRIBUTES_XFER_INT && is_in && iin) {
            *iin = ep->bEndpointAddress;
            if (imps) *imps = USB_EP_DESC_GET_MPS(ep);
        }
    }
}

/* Class-specific descriptors that follow alt 0 of the NCM interface:
   the Ethernet Networking one names the MAC string, the NCM one the
   optional requests the stick takes. */
static void ncm_functional(lte_t *s, const usb_config_desc_t *cd, int off)
{
    const uint8_t *b = (const uint8_t *)cd;
    for (int o = off + b[off]; o + 2 <= cd->wTotalLength && b[o] >= 2; o += b[o]) {
        if (b[o + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) break;
        if (b[o + 1] != 0x24 || b[o] < 3) continue;
        if (b[o + 2] == 0x0f && b[o] >= 13) s->mac_idx = b[o + 3];
        else if (b[o + 2] == 0x1a && b[o] >= 6) s->ncm_caps = b[o + 5];
    }
}

static bool read_mac(lte_t *s)
{
    uint8_t d[64];
    int got = 0;
    if (!s->mac_idx ||
        ctrl(s, REQ_IN_STD_DEV, USB_B_REQUEST_GET_DESCRIPTOR, (0x03 << 8) | s->mac_idx, 0x0409,
             d, sizeof(d), &got) != ESP_OK || got < 2 + 24 || d[1] != 0x03)
        return false;
    for (int i = 0; i < 6; i++) {
        char h[3] = { (char)d[2 + 4 * i], (char)d[4 + 4 * i], 0 };
        char *end;
        s->st.mac[i] = (uint8_t)strtoul(h, &end, 16);
        if (*end) return false;
    }
    return true;
}

static void free_xfer(usb_transfer_t **t, bool busy)
{
    if (!*t) return;
    if (busy) ESP_LOGE(TAG, "transfer still in flight at teardown; leaking it");
    else usb_host_transfer_free(*t);
    *t = NULL;
}

static void stop_eps(lte_t *s, const uint8_t *eps, int n)
{
    for (int i = 0; i < n; i++) {
        if (!eps[i]) continue;
        usb_host_endpoint_halt(s->dev, eps[i]);
        usb_host_endpoint_flush(s->dev, eps[i]);
    }
}

static bool all_idle(lte_t *s)
{
    return !s->at_rx_busy && !s->at_tx_busy && !s->rx_busy[0] && !s->rx_busy[1] &&
           !s->intr_busy && !memchr(s->tx_busy, true, sizeof(s->tx_busy));
}

static void teardown(lte_t *s, const char *why)
{
    if (!s->dev) return;
    ESP_LOGW(TAG, "detach (%s)", why);
    xSemaphoreTake(s->tx_mutex, portMAX_DELAY);
    s->data_ok = false;
    xSemaphoreGive(s->tx_mutex);
    s->tearing_down = true;
    session_down(s, why);
    if (s->netif_started) {
        esp_netif_action_stop(s->netif, NULL, 0, NULL);
        s->netif_started = false;
    }
    ls_at_abort(&s->at, LS_AT_RES_IO);
    if (s->at_done && s->at_owner == OWN_CONSOLE) {
        s->at_done = false;
        memcpy(s->con_resp, s->at.resp, sizeof(s->con_resp));
        s->con_res = LS_AT_RES_IO;
        if (s->con_seq_sent == s->con_seq) xSemaphoreGive(s->con_sem);
    }
    s->at_done = false;

    const uint8_t eps[] = { s->at_claimed ? s->at_in : 0, s->at_claimed ? s->at_out : 0,
                            s->ncm_claimed ? s->ncm_in : 0, s->ncm_claimed ? s->ncm_out : 0,
                            s->ncm_claimed ? s->ncm_int : 0 };
    /* A stick that is gone returns every transfer by itself, and its
       endpoints refuse halt and flush. */
    if (!s->gone && !s->lost) stop_eps(s, eps, sizeof(eps));
    const int64_t end = now_us() + 1500000;
    while (!all_idle(s) && now_us() < end) usb_host_client_handle_events(s->client, pdMS_TO_TICKS(10));
    if (!s->gone && !s->lost)
        for (size_t i = 0; i < sizeof(eps); i++)
            if (eps[i]) usb_host_endpoint_clear(s->dev, eps[i]);
    if (s->at_claimed) usb_host_interface_release(s->client, s->dev, s->at_itf);
    if (s->ncm_claimed) usb_host_interface_release(s->client, s->dev, s->ncm_itf);
    s->at_claimed = s->ncm_claimed = false;
    free_xfer(&s->at_rx, s->at_rx_busy);
    free_xfer(&s->at_tx, s->at_tx_busy);
    for (int i = 0; i < 2; i++) free_xfer(&s->rx[i], s->rx_busy[i]);
    for (int i = 0; i < NTX; i++) free_xfer(&s->tx[i], s->tx_busy[i]);
    free_xfer(&s->intr, s->intr_busy);
    free_xfer(&s->ctrl, false);
    usb_host_device_close(s->client, s->dev);
    s->dev = NULL;
    s->gone = s->lost = false;
    s->tearing_down = false;
    s->st.state = LS_LTE_NONE;
    s->st.have_ip = false;
    /* The deferred work dies with the device; a console waiter is told by
       the aborted command. */
    s->req_dial = s->req_hangup = false;
}

/* Mass-storage mode: the CBW makes the stick drop off and come back as a
   modem. It is gone by the time the CSW would be due, so none is read. */
static void mode_switch(lte_t *s, usb_device_handle_t h, const usb_config_desc_t *cd,
                        uint16_t pid)
{
    int off = 0;
    const usb_intf_desc_t *in = usb_parse_interface_descriptor(cd, 0, 0, &off);
    uint8_t bin = 0, bout = 0;
    if (in) endpoints(cd, in, off, &bin, &bout, NULL, NULL, NULL);
    if (!bout || usb_host_interface_claim(s->client, h, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "12d1:%04x: cannot claim the mass-storage interface", pid);
        return;
    }
    usb_transfer_t *t = NULL;
    bool back = true;
    if (usb_host_transfer_alloc(64, 0, &t) == ESP_OK) {
        t->device_handle = h;
        t->bEndpointAddress = bout;
        t->callback = ctrl_cb;
        t->context = s;
        memcpy(t->data_buffer, k_switch_cbw, sizeof(k_switch_cbw));
        t->num_bytes = sizeof(k_switch_cbw);
        s->ctrl_done = false;
        if (usb_host_transfer_submit(t) == ESP_OK) {
            back = pump_until(s, &s->ctrl_done, 1000);
            if (!back) {
                usb_host_endpoint_halt(h, bout);
                usb_host_endpoint_flush(h, bout);
                back = pump_until(s, &s->ctrl_done, 1000);
                usb_host_endpoint_clear(h, bout);
            }
            ESP_LOGW(TAG, "12d1:%04x: mode switch sent (%s)", pid,
                     back && t->status == USB_TRANSFER_STATUS_COMPLETED ? "taken" : "no answer");
        }
    }
    usb_host_interface_release(s->client, h, 0);
    if (t && back) usb_host_transfer_free(t);
    s->st.state = LS_LTE_SWITCHING;
}

static void go(lte_t *s, ls_lte_state_t st);

static bool attach(lte_t *s, usb_device_handle_t h, const usb_device_desc_t *dd,
                   const usb_config_desc_t *cd)
{
    int at_off = -1, ncm_off = -1, ncm1_off = -1;
    const usb_intf_desc_t *at = NULL, *ncm = NULL, *ncm1 = NULL;
    for (int i = 0; i < cd->bNumInterfaces; i++) {
        int off = 0;
        const usb_intf_desc_t *in = usb_parse_interface_descriptor(cd, i, 0, &off);
        if (!in || in->bInterfaceClass != CLS_VENDOR || in->bInterfaceSubClass != SUB_HUAWEI) continue;
        if (in->bInterfaceProtocol == PROTO_PCUI || (in->bInterfaceProtocol == PROTO_MODEM && !at)) {
            at = in;
            at_off = off;
        } else if (in->bInterfaceProtocol == PROTO_NCM) {
            ncm = in;
            ncm_off = off;
            int o1 = 0;
            ncm1 = usb_parse_interface_descriptor(cd, i, 1, &o1);
            ncm1_off = o1;
        }
    }
    if (!at || !ncm || !ncm1) return false;

    s->dev = h;
    s->addr = 0;
    s->lost = false;
    s->st.last_error[0] = 0;
    s->st.vid = dd->idVendor;
    s->st.pid = dd->idProduct;
    s->at_itf = at->bInterfaceNumber;
    s->at_in = s->at_out = 0;
    endpoints(cd, at, at_off, &s->at_in, &s->at_out, NULL, NULL, NULL);
    s->ncm_itf = ncm->bInterfaceNumber;
    s->ncm_in = s->ncm_out = s->ncm_int = 0;
    s->ncm_mps = 512;
    s->ncm_int_mps = 16;
    endpoints(cd, ncm1, ncm1_off, &s->ncm_in, &s->ncm_out, &s->ncm_int, &s->ncm_mps, &s->ncm_int_mps);
    s->mac_idx = s->ncm_caps = 0;
    ncm_functional(s, cd, ncm_off);
    ESP_LOGW(TAG, "attach %04x:%04x: AT itf %u (0x%02x/0x%02x), NCM itf %u "
                  "(bulk 0x%02x/0x%02x, int 0x%02x), caps 0x%02x",
             dd->idVendor, dd->idProduct, s->at_itf, s->at_out, s->at_in, s->ncm_itf,
             s->ncm_out, s->ncm_in, s->ncm_int, s->ncm_caps);
    s->ctrl = xfer(s, sizeof(usb_setup_packet_t) + 64, 0, ctrl_cb);
    if (!s->ctrl || !s->at_in || !s->at_out || !s->ncm_in || !s->ncm_out) goto fail;

    /* AT port. */
    esp_err_t e = usb_host_interface_claim(s->client, h, s->at_itf, 0);
    if (e != ESP_OK) { ESP_LOGE(TAG, "AT interface claim: %s", esp_err_to_name(e)); goto fail; }
    s->at_claimed = true;
    ep_sync(s, s->at_in);
    ep_sync(s, s->at_out);
    s->at_rx = xfer(s, AT_RX_SIZE, s->at_in, at_rx_cb);
    s->at_tx = xfer(s, 512, s->at_out, at_tx_cb);
    if (!s->at_rx || !s->at_tx) goto fail;
    ls_at_init(&s->at, at_urc, at_done, s);
    at_rx_post(s);

    /* NCM: reset the data alt, read and set the NTB parameters while it
       is idle, then select alt 1 (IDF's claim does not send it). */
    ctrl(s, REQ_OUT_STD_ITF, USB_B_REQUEST_SET_INTERFACE, 0, s->ncm_itf, NULL, 0, NULL);
    uint8_t p[LS_NCM_PARAMS_LEN];
    int got = 0;
    e = ctrl(s, REQ_IN_CLS_ITF, NCM_GET_NTB_PARAMETERS, 0, s->ncm_itf, p, sizeof(p), &got);
    if (e != ESP_OK || !ls_ncm_parse_params(p, got, &s->ntb)) {
        ESP_LOGE(TAG, "GET_NTB_PARAMETERS: %s", esp_err_to_name(e));
        goto fail;
    }
    ESP_LOGW(TAG, "NTB params: formats 0x%x in max %lu div %u rem %u align %u, "
                  "out max %lu div %u rem %u align %u max dgrams %u",
             s->ntb.formats, (unsigned long)s->ntb.in_max, s->ntb.in_divisor, s->ntb.in_remainder,
             s->ntb.in_align, (unsigned long)s->ntb.out_max, s->ntb.out_divisor,
             s->ntb.out_remainder, s->ntb.out_align, s->ntb.out_max_datagrams);
    /* Only a stick that also does NTB32 needs telling; others may stall. */
    if (s->ntb.formats & 2)
        ctrl(s, REQ_OUT_CLS_ITF, NCM_SET_NTB_FORMAT, 0, s->ncm_itf, NULL, 0, NULL);
    if (s->ncm_caps & 0x10)
        ctrl(s, REQ_OUT_CLS_ITF, NCM_SET_CRC_MODE, 0, s->ncm_itf, NULL, 0, NULL);
    uint32_t in_size = s->ntb.in_max && s->ntb.in_max < RX_NTB_CAP ? s->ntb.in_max : RX_NTB_CAP;
    if (in_size != s->ntb.in_max) {
        uint8_t v[4] = { (uint8_t)in_size, (uint8_t)(in_size >> 8), (uint8_t)(in_size >> 16),
                         (uint8_t)(in_size >> 24) };
        e = ctrl(s, REQ_OUT_CLS_ITF, NCM_SET_NTB_INPUT_SIZE, 0, s->ncm_itf, v, 4, NULL);
        if (e != ESP_OK) {
            /* It keeps its own maximum then, and our buffers must hold it. */
            ESP_LOGW(TAG, "SET_NTB_INPUT_SIZE(%lu): %s", (unsigned long)in_size, esp_err_to_name(e));
            in_size = s->ntb.in_max;
        }
    }
    s->st.ntb_in = in_size;
    s->st.ntb_out = s->ntb.out_max && s->ntb.out_max < TX_NTB_CAP ? s->ntb.out_max : TX_NTB_CAP;
    if (s->ncm_caps & 0x01)
        ctrl(s, REQ_OUT_CLS_ITF, CDC_SET_PACKET_FILTER, 0x0c /* directed | broadcast */,
             s->ncm_itf, NULL, 0, NULL);
    if (read_mac(s)) {
        ESP_LOGW(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x (string %u)", s->st.mac[0], s->st.mac[1],
                 s->st.mac[2], s->st.mac[3], s->st.mac[4], s->st.mac[5], s->mac_idx);
    } else {
        esp_efuse_mac_get_default(s->st.mac);
        s->st.mac[0] = (s->st.mac[0] | 0x02) & ~0x01;
        s->st.mac[5] ^= 0x4c;
        ESP_LOGW(TAG, "no MAC string; using a local one");
    }
    e = usb_host_interface_claim(s->client, h, s->ncm_itf, 1);
    if (e != ESP_OK) { ESP_LOGE(TAG, "NCM interface claim: %s", esp_err_to_name(e)); goto fail; }
    s->ncm_claimed = true;
    e = ctrl(s, REQ_OUT_STD_ITF, USB_B_REQUEST_SET_INTERFACE, 1, s->ncm_itf, NULL, 0, NULL);
    if (e != ESP_OK) { ESP_LOGE(TAG, "SET_INTERFACE alt 1: %s", esp_err_to_name(e)); goto fail; }
    ep_sync(s, s->ncm_in);
    ep_sync(s, s->ncm_out);
    const size_t rx_size = (in_size + s->ncm_mps - 1) / s->ncm_mps * s->ncm_mps;
    for (int i = 0; i < 2; i++)
        if (!(s->rx[i] = xfer(s, rx_size, s->ncm_in, rx_cb))) goto fail;
    for (int i = 0; i < NTX; i++)
        if (!(s->tx[i] = xfer(s, s->st.ntb_out, s->ncm_out, tx_cb))) goto fail;
    if (s->ncm_int) s->intr = xfer(s, s->ncm_int_mps, s->ncm_int, intr_cb);
    ESP_LOGW(TAG, "NTB buffers: in %u x2, out %lu x%d, in %s", (unsigned)rx_size,
             (unsigned long)s->st.ntb_out, NTX,
             esp_ptr_external_ram(s->rx[0]->data_buffer) ? "PSRAM" : "internal RAM");
    rx_post(s, 0);
    rx_post(s, 1);
    intr_post(s);

    if (netif_create(s) != ESP_OK) { ESP_LOGE(TAG, "netif create failed"); goto fail; }
    esp_netif_set_mac(s->netif, s->st.mac);
    esp_netif_action_start(s->netif, NULL, 0, NULL);
    s->netif_started = true;
    xSemaphoreTake(s->tx_mutex, portMAX_DELAY);
    s->data_ok = true;
    xSemaphoreGive(s->tx_mutex);
    s->st.model[0] = s->st.revision[0] = s->st.imei[0] = s->st.oper[0] = s->st.tech[0] = 0;
    ls_at_signal_clear(&s->st.sig);
    s->hold = false;
    go(s, LS_LTE_PROBING);
    return true;
fail:
    teardown(s, "attach failed");
    return true; /* ours, just unusable; do not hand it on */
}

static void new_device(lte_t *s, uint8_t addr)
{
    usb_device_handle_t h;
    if (usb_host_device_open(s->client, addr, &h) != ESP_OK) return;
    const usb_device_desc_t *dd = NULL;
    const usb_config_desc_t *cd = NULL;
    if (usb_host_get_device_descriptor(h, &dd) != ESP_OK ||
        usb_host_get_active_config_descriptor(h, &cd) != ESP_OK || dd->idVendor != HUAWEI_VID) {
        usb_host_device_close(s->client, h);
        return;
    }
    s->seen_us = now_us();
    int off = 0;
    const usb_intf_desc_t *i0 = usb_parse_interface_descriptor(cd, 0, 0, &off);
    if (i0 && i0->bInterfaceClass == USB_CLASS_MASS_STORAGE) {
        ESP_LOGW(TAG, "12d1:%04x at addr %u in mass-storage mode", dd->idProduct, addr);
        mode_switch(s, h, cd, dd->idProduct);
        usb_host_device_close(s->client, h);
        return;
    }
    if (s->dev || !attach(s, h, dd, cd)) {
        usb_host_device_close(s->client, h);
        return;
    }
    s->addr = addr;
}

static void scan_port(lte_t *s)
{
    uint8_t a[8];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(a), a, &n) != ESP_OK) return;
    for (int i = 0; i < n && !s->dev; i++) new_device(s, a[i]);
}

/* ------------------------------------------------------- state machine */

static void go(lte_t *s, ls_lte_state_t st)
{
    if (s->st.state != st)
        ESP_LOGW(TAG, "%s -> %s", ls_lte_state_name(s->st.state), ls_lte_state_name(st));
    s->st.state = st;
    s->step = 0;
    s->tries = 0;
    s->t_enter = now_us();
    s->wake = 0;
}

static const char *apn_in_use(lte_t *s)
{
    return s->apn_cfg[0] ? s->apn_cfg : s->profile_apn;
}

static void first_line(const char *resp, char *out, size_t cap)
{
    size_t n = strcspn(resp, "\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, resp, n);
    out[n] = 0;
}

static void apply_dhcp(lte_t *s, const char *resp)
{
    ls_at_dhcp_t d;
    if (!ls_at_parse_dhcp(resp, &d)) {
        ESP_LOGE(TAG, "^DHCP? unreadable");
        return;
    }
    esp_netif_ip_info_t ip;
    memcpy(&ip.ip.addr, d.ip, 4);
    memcpy(&ip.netmask.addr, d.mask, 4);
    memcpy(&ip.gw.addr, d.gw, 4);
    esp_netif_dhcpc_stop(s->netif);
    /* DNS before the address: the GOT_IP that set_ip_info posts is where
       the route policy notes this interface's servers. */
    for (int i = 0; i < 2; i++) {
        esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4 };
        memcpy(&dns.ip.u_addr.ip4.addr, i ? d.dns2 : d.dns1, 4);
        if (dns.ip.u_addr.ip4.addr)
            esp_netif_set_dns_info(s->netif, i ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_MAIN, &dns);
    }
    esp_netif_set_ip_info(s->netif, &ip);
    s->st.static_ip = true;
    ESP_LOGW(TAG, "no DHCP reply in 15 s: static from ^DHCP: %u.%u.%u.%u/%u.%u.%u.%u gw "
                  "%u.%u.%u.%u", d.ip[0], d.ip[1], d.ip[2], d.ip[3], d.mask[0], d.mask[1],
             d.mask[2], d.mask[3], d.gw[0], d.gw[1], d.gw[2], d.gw[3]);
}

/* What the reply to the command sent in (state, step, special) means. */
static void on_reply(lte_t *s, ls_at_result_t res, const char *resp)
{
    const bool ok = res == LS_AT_RES_OK;
    int v;
    switch (s->cmd_special) {
    case SP_HANGUP:
        session_down(s, "hangup");
        s->hold = true;
        go(s, LS_LTE_REGISTERED);
        s->step = 2;
        return;
    case SP_CSQ:
        if (ok && ls_at_parse_csq(resp, &v)) s->st.sig.rssi_dbm = v;
        s->last_sig = now_us();
        return;
    case SP_DHCP:
        if (ok && s->st.state == LS_LTE_CONNECTED && !s->st.have_ip) apply_dhcp(s, resp);
        return;
    case SP_NDISQ:
        if (ok && ls_at_parse_ndisstat(resp, &v) && v == 0) s->urc_ndis_down = true;
        return;
    case SP_NONE:
        break;
    }
    /* The state moved on while the command was out (a URC, a request). */
    if (s->cmd_state != s->st.state || s->cmd_step != s->step) return;

    switch (s->st.state) {
    case LS_LTE_PROBING:
        if (ok) go(s, LS_LTE_INIT);
        else if (++s->tries >= 10) {
            set_error(s, "AT port silent%s", "");
            go(s, LS_LTE_ERROR);
        } else s->wake = now_us() + 1000000;
        break;
    case LS_LTE_INIT:
        if (s->step == 1 && ok) {
            ls_at_parse_ati(resp, s->st.model, sizeof(s->st.model), s->st.revision,
                            sizeof(s->st.revision), s->st.imei, sizeof(s->st.imei));
            ESP_LOGW(TAG, "%s rev %s", s->st.model, s->st.revision);
        }
        if (s->step == 2) {
            if (!ok || !strstr(resp, "READY")) {
                /* "SIM busy" right after power-up is worth a few retries. */
                if (++s->tries < 5) { s->wake = now_us() + 2000000; break; }
                first_line(resp, s->st.last_error, sizeof(s->st.last_error));
                ESP_LOGE(TAG, "SIM: %s", s->st.last_error);
                go(s, LS_LTE_SIM_ERROR);
                break;
            }
        }
        if (s->step == 3) {
            s->profile_apn[0] = 0;
            if (ok) ls_at_parse_cgdcont_apn(resp, 1, s->profile_apn, sizeof(s->profile_apn));
            go(s, LS_LTE_SEARCHING);
            break;
        }
        s->step++;
        break;
    case LS_LTE_SEARCHING:
    case LS_LTE_NO_SERVICE:
        if (s->step == 0) {
            s->step = ok && ls_at_parse_reg(resp, "+CEREG:", &v) && (v == 1 || v == 5) ? 2 : 1;
        } else if (s->step == 1) {
            char mode[12];
            if (ok && ls_at_parse_sysinfoex(resp, &v, mode, sizeof(mode)) && v == 2) {
                s->step = 2;
            } else {
                if (s->st.state == LS_LTE_SEARCHING && now_us() - s->t_enter > REG_TIMEOUT_US) {
                    set_error(s, "not registered after 90 s%s", "");
                    const int64_t t = s->t_enter;
                    go(s, LS_LTE_NO_SERVICE);
                    s->t_enter = t;
                }
                s->step = 0;
                s->wake = now_us() + 2000000;
            }
        } else if (s->step == 2) {
            if (ok) ls_at_parse_cops(resp, s->st.oper, sizeof(s->st.oper), &s->st.act);
            s->step = 3;
        } else {
            char mode[12];
            if (ok && ls_at_parse_sysinfoex(resp, &v, mode, sizeof(mode)) && mode[0])
                snprintf(s->st.tech, sizeof(s->st.tech), "%s", mode);
            ESP_LOGW(TAG, "registered: %s, %s", s->st.oper, s->st.tech);
            go(s, LS_LTE_REGISTERED);
        }
        break;
    case LS_LTE_REGISTERED:
        if (s->step == 0) {
            if (ok && ls_at_parse_reg(resp, "+CEREG:", &v) && v != 1 && v != 5) go(s, LS_LTE_SEARCHING);
            else s->step = 1;
        } else if (s->step == 1) {
            if (ok && ls_at_parse_ndisstat(resp, &v) && v == 1) {
                /* Still up from before a reboot of ours: take it over. */
                ESP_LOGW(TAG, "data session already up; adopting it");
                go(s, LS_LTE_CONNECTED);
            } else s->step = 2;
        }
        break;
    case LS_LTE_DIALING:
        if (s->step == 0) {
            if (!ok) {
                first_line(resp, s->st.last_error, sizeof(s->st.last_error));
                ESP_LOGE(TAG, "NDISDUP: %s", s->st.last_error);
                go(s, LS_LTE_HUNGUP);
                s->wake = now_us() + REDIAL_US;
            } else {
                s->step = 1;
                s->t_dial = now_us();
                s->wake = now_us() + 2000000;
            }
        } else {
            if (ok && ls_at_parse_ndisstat(resp, &v) && v == 1) go(s, LS_LTE_CONNECTED);
            else if (now_us() - s->t_dial > DIAL_TIMEOUT_US) {
                set_error(s, "no data session after 30 s%s", "");
                ESP_LOGE(TAG, "%s", s->st.last_error);
                go(s, LS_LTE_HUNGUP);
                s->wake = now_us() + REDIAL_US;
            } else s->wake = now_us() + 2000000;
        }
        break;
    default:
        break;
    }
}

/* Next command (or action) of the current state, when nothing is out. */
static void run_state(lte_t *s)
{
    const int64_t now = now_us();
    char cmd[LS_AT_CMD_MAX];
    if (s->req_hangup) {
        s->req_hangup = false;
        if (s->st.state == LS_LTE_CONNECTED || s->st.state == LS_LTE_DIALING ||
            s->st.state == LS_LTE_HUNGUP || s->st.state == LS_LTE_REGISTERED) {
            at_send(s, "AT^NDISDUP=1,0", AT_TIMEOUT_MS, OWN_SELF, SP_HANGUP);
            return;
        }
    }
    if (s->req_dial) {
        s->req_dial = false;
        s->hold = false;
        if (s->st.state == LS_LTE_HUNGUP || (s->st.state == LS_LTE_REGISTERED && s->step == 2))
            go(s, LS_LTE_REGISTERED);
    }
    if (now < s->wake) return;
    const bool registered = s->st.state >= LS_LTE_REGISTERED && s->st.state <= LS_LTE_HUNGUP;
    if (registered && now - s->last_sig > SIG_POLL_US && now - s->last_poll > SIG_POLL_US) {
        s->last_poll = now;
        at_send(s, "AT+CSQ", AT_TIMEOUT_MS, OWN_SELF, SP_CSQ);
        return;
    }
    switch (s->st.state) {
    case LS_LTE_PROBING:
        at_send(s, "AT", 1000, OWN_SELF, SP_NONE);
        break;
    case LS_LTE_INIT: {
        static const char *const init[] = { "ATE0", "ATI", "AT+CPIN?", "AT+CGDCONT?" };
        at_send(s, init[s->step], AT_TIMEOUT_MS, OWN_SELF, SP_NONE);
        break;
    }
    case LS_LTE_SEARCHING:
    case LS_LTE_NO_SERVICE: {
        static const char *const reg[] = { "AT+CEREG?", "AT^SYSINFOEX", "AT+COPS?", "AT^SYSINFOEX" };
        at_send(s, reg[s->step], AT_TIMEOUT_MS, OWN_SELF, SP_NONE);
        break;
    }
    case LS_LTE_REGISTERED:
        if (s->step == 0) at_send(s, "AT+CEREG?", AT_TIMEOUT_MS, OWN_SELF, SP_NONE);
        else if (s->step == 1) at_send(s, "AT^NDISSTATQRY?", AT_TIMEOUT_MS, OWN_SELF, SP_NONE);
        else if (s->auto_dial && !s->hold) go(s, LS_LTE_DIALING);
        else s->wake = now + 500000;
        break;
    case LS_LTE_DIALING:
        if (s->step == 0) {
            if (!apn_in_use(s)[0]) {
                set_error(s, "no APN: set one with 'lte apn'%s", "");
                go(s, LS_LTE_REGISTERED);
                s->step = 2;
                s->hold = true;
                break;
            }
            snprintf(s->st.apn, sizeof(s->st.apn), "%s", apn_in_use(s));
            snprintf(cmd, sizeof(cmd), "AT^NDISDUP=1,1,\"%s\"", s->st.apn);
            ESP_LOGW(TAG, "dial: %s", cmd);
            s->urc_ndis_up = false;
            at_send(s, cmd, 10000, OWN_SELF, SP_NONE);
        } else if (s->urc_ndis_up) {
            go(s, LS_LTE_CONNECTED);
        } else {
            at_send(s, "AT^NDISSTATQRY?", AT_TIMEOUT_MS, OWN_SELF, SP_NONE);
        }
        break;
    case LS_LTE_CONNECTED:
        if (s->step == 0) {
            ESP_LOGW(TAG, "data session up; starting DHCP");
            session_up(s);
            s->step = 1;
            s->last_poll = now;
            break;
        }
        if (!s->st.have_ip && !s->dhcp_fallback_tried && now - s->t_conn > DHCP_WAIT_US) {
            s->dhcp_fallback_tried = true;
            at_send(s, "AT^DHCP?", AT_TIMEOUT_MS, OWN_SELF, SP_DHCP);
            break;
        }
        /* The ^NDISSTAT:0 URC is the normal news of a drop; this catches
           one that never arrived. */
        if (now - s->last_poll > SIG_POLL_US) {
            s->last_poll = now;
            at_send(s, "AT^NDISSTATQRY?", AT_TIMEOUT_MS, OWN_SELF, SP_NDISQ);
            break;
        }
        s->wake = now + 1000000;
        break;
    case LS_LTE_HUNGUP:
        if (s->auto_dial && !s->hold) go(s, LS_LTE_REGISTERED);
        else s->wake = now + 1000000;
        break;
    default:
        s->wake = now + 1000000;
        break;
    }
}

static void note_ip(lte_t *s)
{
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s->netif, &ip) != ESP_OK) return;
    s->st.ip = ip.ip.addr;
    s->st.mask = ip.netmask.addr;
    s->st.gw = ip.gw.addr;
    esp_netif_dns_info_t d[2];
    if (ls_net_dns_of(s->netif, d)) {
        s->st.dns[0] = d[0].ip.u_addr.ip4.addr;
        s->st.dns[1] = d[1].ip.u_addr.ip4.addr;
    }
    s->st.have_ip = true;
    s->st.last_error[0] = 0;
#define Q(a) (unsigned)((a) & 0xff), (unsigned)((a) >> 8 & 0xff), (unsigned)((a) >> 16 & 0xff), (unsigned)((a) >> 24)
    ESP_LOGW(TAG, "got IP %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u %u.%u.%u.%u (%s)",
             Q(s->st.ip), Q(s->st.mask), Q(s->st.gw), Q(s->st.dns[0]), Q(s->st.dns[1]),
             s->st.static_ip ? "static from ^DHCP" : "DHCP");
#undef Q
}

static void tick(lte_t *s)
{
    if (s->gone) teardown(s, "device gone");
    while (s->new_n) {
        const uint8_t a = s->new_addr[--s->new_n];
        new_device(s, a);
    }
    if (s->scan_existing) {
        s->scan_existing = false;
        scan_port(s);
    }
    if (s->req_switch) {
        s->req_switch = false;
        if (!s->dev) scan_port(s);
        else ESP_LOGW(TAG, "the stick is already in modem mode");
    }
    if (!s->dev) return;
    s->seen_us = now_us();
    at_rx_post(s); /* after an error, the AT reader is reposted here */
    rx_post(s, 0);
    rx_post(s, 1);
    intr_post(s);

    if (s->ev_got_ip) { s->ev_got_ip = false; if (s->netif_connected) note_ip(s); }
    if (s->ev_lost_ip) { s->ev_lost_ip = false; s->st.have_ip = false; }
    if (s->urc_ndis_down) {
        s->urc_ndis_down = false;
        if (s->st.state == LS_LTE_CONNECTED || s->st.state == LS_LTE_DIALING) {
            session_down(s, "network hung up");
            set_error(s, "session dropped by the network%s", "");
            go(s, LS_LTE_HUNGUP);
            s->wake = now_us() + REDIAL_US;
        }
    }
    if (s->urc_ndis_up && s->st.state == LS_LTE_DIALING && s->step == 1) s->wake = 0;

    if (s->at.busy && now_us() > s->at_deadline) ls_at_abort(&s->at, LS_AT_RES_TIMEOUT);
    if (s->at_done) {
        s->at_done = false;
        if (s->at_owner == OWN_CONSOLE) {
            taskENTER_CRITICAL(&s_lock);
            memcpy(s->con_resp, s->at.resp, sizeof(s->con_resp));
            s->con_res = s->at_res;
            const bool mine = s->con_seq_sent == s->con_seq;
            taskEXIT_CRITICAL(&s_lock);
            if (mine) xSemaphoreGive(s->con_sem);
        } else {
            if (s->at_res != LS_AT_RES_OK && s->cmd_special == SP_NONE)
                ESP_LOGD(TAG, "%s: %s", s->at.cmd, ls_at_result_name(s->at_res));
            on_reply(s, s->at_res, s->at.resp);
        }
    }
    if (s->at.busy || s->at_tx_busy) return;
    if (s->con_pending && s->st.state != LS_LTE_SWITCHING) {
        char cmd[LS_AT_CMD_MAX];
        taskENTER_CRITICAL(&s_lock);
        memcpy(cmd, s->con_cmd, sizeof(cmd));
        s->con_pending = false;
        s->con_seq_sent = s->con_seq;
        taskEXIT_CRITICAL(&s_lock);
        if (!at_send(s, cmd, AT_TIMEOUT_MS, OWN_CONSOLE, SP_NONE)) {
            s->con_res = LS_AT_RES_IO;
            s->con_resp[0] = 0;
            xSemaphoreGive(s->con_sem);
        }
        return;
    }
    run_state(s);
}

static void lte_task(void *arg)
{
    lte_t *s = arg;
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_cb, .callback_arg = s },
    };
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 10 && e != ESP_OK; i++) {
        e = usb_host_client_register(&cfg, &s->client);
        if (e != ESP_OK) vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "USB client register: %s - LTE unavailable", esp_err_to_name(e));
        vTaskDelete(NULL);
        return;
    }
    /* A stick enumerated before this client existed sent its NEW_DEV to
       nobody here. */
    s->scan_existing = true;
    for (;;) {
        usb_host_client_handle_events(s->client, pdMS_TO_TICKS(TICK_MS));
        tick(s);
    }
}

/* ------------------------------------------------------------------ API */

esp_err_t ls_lte_start(const ls_lte_config_t *cfg)
{
    if (s_lte) return ESP_ERR_INVALID_STATE;
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    lte_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) return ESP_ERR_NO_MEM;
    s->tx_mutex = xSemaphoreCreateMutex();
    s->con_mutex = xSemaphoreCreateMutex();
    s->con_sem = xSemaphoreCreateBinary();
    if (!s->tx_mutex || !s->con_mutex || !s->con_sem) return ESP_ERR_NO_MEM;
    snprintf(s->apn_cfg, sizeof(s->apn_cfg), "%s", cfg ? cfg->apn : "");
    s->auto_dial = cfg ? cfg->auto_dial : true;
    s->st.auto_dial = s->auto_dial;
    ls_at_signal_clear(&s->st.sig);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event, s);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, ip_event, s);
    ls_net_init();
    s_lte = s;
    /* Internal-RAM stack: this task calls into the USB host and esp_netif,
       and must not stall while flash is busy. */
    if (xTaskCreatePinnedToCore(lte_task, "lte", 5120, s, 13, NULL, 1) != pdPASS) {
        s_lte = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool ls_lte_has_device(void)
{
    lte_t *s = s_lte;
    return s && (s->dev || (s->seen_us && esp_timer_get_time() - s->seen_us < SEEN_HOLD_US));
}

void ls_lte_get_status(ls_lte_status_t *out)
{
    lte_t *s = s_lte;
    if (!s) { memset(out, 0, sizeof(*out)); return; }
    taskENTER_CRITICAL(&s_lock);
    *out = s->st;
    snprintf(out->apn, sizeof(out->apn), "%s", s->st.state == LS_LTE_CONNECTED ? s->st.apn : apn_in_use(s));
    out->auto_dial = s->auto_dial;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t ls_lte_at(const char *cmd, char *resp, size_t cap, uint32_t timeout_ms,
                    ls_at_result_t *res)
{
    lte_t *s = s_lte;
    if (!s) return ESP_ERR_INVALID_STATE;
    if (strlen(cmd) >= LS_AT_CMD_MAX) return ESP_ERR_INVALID_SIZE;
    if (!s->dev) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s->con_mutex, portMAX_DELAY);
    xSemaphoreTake(s->con_sem, 0);
    taskENTER_CRITICAL(&s_lock);
    snprintf(s->con_cmd, sizeof(s->con_cmd), "%s", cmd);
    s->con_seq++;
    s->con_pending = true;
    taskEXIT_CRITICAL(&s_lock);
    if (s->client) usb_host_client_unblock(s->client);
    esp_err_t e = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(s->con_sem, pdMS_TO_TICKS(timeout_ms + 1000)) == pdTRUE) {
        taskENTER_CRITICAL(&s_lock);
        if (resp && cap) snprintf(resp, cap, "%s", s->con_resp);
        if (res) *res = s->con_res;
        taskEXIT_CRITICAL(&s_lock);
        e = ESP_OK;
    } else {
        taskENTER_CRITICAL(&s_lock);
        s->con_pending = false; /* if it never went out */
        s->con_seq++;           /* a late reply is not for the next caller */
        taskEXIT_CRITICAL(&s_lock);
    }
    xSemaphoreGive(s->con_mutex);
    return e;
}

static void poke(void)
{
    if (s_lte && s_lte->client) usb_host_client_unblock(s_lte->client);
}

void ls_lte_dial(void) { if (s_lte) { s_lte->req_dial = true; poke(); } }
void ls_lte_hangup(void) { if (s_lte) { s_lte->req_hangup = true; poke(); } }
void ls_lte_force_switch(void) { if (s_lte) { s_lte->req_switch = true; poke(); } }

void ls_lte_set_apn(const char *apn)
{
    if (!s_lte) return;
    taskENTER_CRITICAL(&s_lock);
    snprintf(s_lte->apn_cfg, sizeof(s_lte->apn_cfg), "%s", apn ? apn : "");
    taskEXIT_CRITICAL(&s_lock);
}

void ls_lte_set_auto(bool on)
{
    if (!s_lte) return;
    taskENTER_CRITICAL(&s_lock);
    s_lte->auto_dial = on;
    s_lte->st.auto_dial = on;
    taskEXIT_CRITICAL(&s_lock);
    poke();
}
