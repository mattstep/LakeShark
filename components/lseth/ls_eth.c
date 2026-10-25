/* See ls_eth.h and docs/ETHERNET.md. */
#include "ls_eth.h"

#include <string.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "ls_board.h"
#include "ls_caps.h"
#include "ls_nvs_safe.h"
#if LS_HAS_IO_EXPANDER && defined(LS_BOARD_XL_ETH_PHY_RST)
#include "ls_xl9535.h"
#define PHY_RST_VIA_EXPANDER 1
#else
#define PHY_RST_VIA_EXPANDER 0
#endif

#if !defined(LS_BOARD_ETH_PHY_ADDR) || !defined(LS_BOARD_ETH_TXD1_GPIO)
#error "CONFIG_LS_ETH needs LS_BOARD_ETH_* pins in the board variant"
#endif

static const char *TAG = "eth";

/* IP101GRI datasheet, Table 15: reset held low for Trst >= 10 ms, and the
   RMII clock is valid Tclk_MII_rdy <= 10 ms after release (the chip itself
   is usable 2.5 ms after).  The EMAC's software reset does not finish
   without that clock, so the settle time comes before esp_eth_mac_new. */
#define PHY_RESET_HOLD_MS    15
#define PHY_RESET_SETTLE_MS  20

/* The rx task copies a frame out of the DMA ring and hands it to lwIP's
   mailbox; it never runs the stack itself.  3 KB instead of IDF's 4 KB is
   a kilobyte of internal RAM; `eth` prints its high-water mark. */
#define RX_TASK_STACK        3072

#define NVS_NS  "lseth"
#define NVS_KEY "en"

static esp_eth_mac_t *s_mac;
static esp_eth_phy_t *s_phy;
static esp_eth_handle_t s_eth;
static esp_eth_netif_glue_handle_t s_glue;
static esp_netif_t *s_netif;
static esp_event_handler_instance_t s_eth_evt, s_got_ip_evt, s_lost_ip_evt;

/* Written from the event loop, read from the console. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ls_eth_status_t s_st;

/* ------------------------------------------------------------ PHY reset */

static esp_err_t phy_hold_reset(void)
{
#if PHY_RST_VIA_EXPANDER
    if (!ls_xl9535_ready()) return ESP_ERR_INVALID_STATE;
    return ls_xl9535_out(LS_BOARD_XL_ETH_PHY_RST, false);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t phy_reset_pulse(void)
{
#if PHY_RST_VIA_EXPANDER
    /* Without the expander the PHY's own pull-up has it running since power
       on, at an unknown point in its strap latch; a clean pulse is the only
       known state, and there is no other way to reach the pin. */
    if (!ls_xl9535_ready()) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(ls_xl9535_out(LS_BOARD_XL_ETH_PHY_RST, false), TAG,
                        "PHY reset assert");
    vTaskDelay(pdMS_TO_TICKS(PHY_RESET_HOLD_MS));
    ESP_RETURN_ON_ERROR(ls_xl9535_set(LS_BOARD_XL_ETH_PHY_RST, true), TAG,
                        "PHY reset release");
    vTaskDelay(pdMS_TO_TICKS(PHY_RESET_SETTLE_MS));
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

/* --------------------------------------------------------------- events */

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id,
                         void *data)
{
    (void)arg;
    (void)base;
    if (!data || *(esp_eth_handle_t *)data != s_eth) return;

    if (id == ETHERNET_EVENT_CONNECTED) {
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
        const int mbps = speed == ETH_SPEED_100M ? 100 : 10;
        const bool full = duplex == ETH_DUPLEX_FULL;
        portENTER_CRITICAL(&s_lock);
        s_st.link_up = true;
        s_st.speed_mbps = mbps;
        s_st.full_duplex = full;
        s_st.link_changes++;
        s_st.link_since_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_lock);
        /* WARN: the shipped log level, and a cable going in is news. */
        ESP_LOGW(TAG, "link up %d Mbps %s duplex", mbps, full ? "full" : "half");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        portENTER_CRITICAL(&s_lock);
        s_st.link_up = false;
        s_st.has_ip = false;
        s_st.speed_mbps = 0;
        s_st.link_changes++;
        s_st.link_since_us = 0;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "link down");
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id,
                        void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        if (!ev || ev->esp_netif != s_netif) return;
        portENTER_CRITICAL(&s_lock);
        s_st.has_ip = true;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "got " IPSTR " mask " IPSTR " gw " IPSTR,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask),
                 IP2STR(&ev->ip_info.gw));
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        const ip_event_got_ip_t *ev = data;
        if (!ev || ev->esp_netif != s_netif) return;
        portENTER_CRITICAL(&s_lock);
        s_st.has_ip = false;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "lost IP");
    }
}

/* ------------------------------------------------------------- bring-up */

/* The stored setting, cached by load and save. */
static bool s_setting = true;
/* One switch at a time: the console and the Settings screen can both ask. */
static volatile bool s_busy;
static ls_eth_pin_hook_t s_pin_hook;

void ls_eth_set_pin_hook(ls_eth_pin_hook_t fn) { s_pin_hook = fn; }

static void pin_hook(bool eth_owns)
{
    if (s_pin_hook) s_pin_hook(eth_owns);
}

static bool claim(void)
{
    bool ok = false;
    portENTER_CRITICAL(&s_lock);
    if (!s_busy) { s_busy = true; ok = true; }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

static void release(void) { s_busy = false; }

/* Undoes as much of a start as happened, in IDF's own order, so it serves a
   start that failed half way and a stop alike. ESP_ERR_INVALID_STATE when
   the driver would not uninstall: it and the EMAC are then left installed
   (deleting the MAC under it would be worse) and only a reboot clears it. */
static esp_err_t teardown(void)
{
    /* esp_event holds the loop's lock while a handler runs, so once these
       return neither handler is running or will run again. */
    if (s_eth_evt)
        esp_event_handler_instance_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, s_eth_evt);
    if (s_got_ip_evt)
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, s_got_ip_evt);
    if (s_lost_ip_evt)
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, s_lost_ip_evt);
    s_eth_evt = s_got_ip_evt = s_lost_ip_evt = NULL;

    if (s_eth && s_st.started) {
        esp_eth_stop(s_eth);
        /* A frame the rx task already holds must not reach a netif that is
           about to be freed: cut the input path (public API, the driver then
           drops frames) and give the task, priority 15 on either core, a
           moment to finish the one in hand. */
        esp_eth_update_input_path_info(s_eth, NULL, NULL);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    /* The glue holds a reference on the driver and uninstall refuses while
       one is outstanding. esp_netif_destroy runs on the tcpip thread and
       returns once lwIP has let go of the netif, so nothing transmits on
       the driver after it. */
    if (s_glue) esp_eth_del_netif_glue(s_glue);
    s_glue = NULL;
    if (s_netif) esp_netif_destroy(s_netif);
    s_netif = NULL;
    if (s_eth) {
        const esp_err_t e = esp_eth_driver_uninstall(s_eth);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "driver uninstall: %s - Ethernet stays half down "
                          "until reboot", esp_err_to_name(e));
            return ESP_ERR_INVALID_STATE;
        }
    }
    s_eth = NULL;
    /* mac->del also returns every EMAC pin, GPIO35 among them, to GPIO. */
    if (s_mac) s_mac->del(s_mac);
    if (s_phy) s_phy->del(s_phy);
    s_mac = NULL;
    s_phy = NULL;
    (void)phy_hold_reset();

    portENTER_CRITICAL(&s_lock);
    s_st.started = false;
    s_st.link_up = false;
    s_st.has_ip = false;
    s_st.speed_mbps = 0;
    s_st.link_since_us = 0;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

static esp_err_t fail(esp_err_t err, const char *what)
{
    ESP_LOGE(TAG, "%s: %s - Ethernet off, BOOT button back", what, esp_err_to_name(err));
    if (teardown() == ESP_OK) pin_hook(false);
    portENTER_CRITICAL(&s_lock);
    s_st.start_err = err;
    portEXIT_CRITICAL(&s_lock);
    return err;
}

static esp_err_t bring_up(void)
{
    if (s_eth) return ESP_OK;
    s_st.phy_addr = LS_BOARD_ETH_PHY_ADDR;

    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return fail(e, "esp_netif_init");
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return fail(e, "event loop");

    /* GPIO35's other user lets go before the EMAC claims it. */
    pin_hook(true);

    e = phy_reset_pulse();
    if (e != ESP_OK) return fail(e, "PHY reset via the IO expander");

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = RX_TASK_STACK;
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = LS_BOARD_ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = LS_BOARD_ETH_MDIO_GPIO;
    emac_cfg.interface = EMAC_DATA_INTERFACE_RMII;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = LS_BOARD_ETH_REF_CLK_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.tx_en_num = LS_BOARD_ETH_TX_EN_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.txd0_num = LS_BOARD_ETH_TXD0_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.txd1_num = LS_BOARD_ETH_TXD1_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.crs_dv_num = LS_BOARD_ETH_CRS_DV_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.rxd0_num = LS_BOARD_ETH_RXD0_GPIO;
    emac_cfg.emac_dataif_gpio.rmii.rxd1_num = LS_BOARD_ETH_RXD1_GPIO;
    s_mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    if (!s_mac) return fail(ESP_ERR_NO_MEM, "EMAC (DMA buffers are internal RAM)");

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = LS_BOARD_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = -1;    /* the expander did it above */
    s_phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (!s_phy) return fail(ESP_ERR_NO_MEM, "IP101 driver");

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(s_mac, s_phy);
    e = esp_eth_driver_install(&eth_cfg, &s_eth);
    if (e != ESP_OK) {
        /* install leaves the handle unset on failure; nothing to uninstall. */
        s_eth = NULL;
        ESP_LOGE(TAG, "no IP101 answering at MDIO address %d (MDC %d, MDIO %d, "
                      "REF_CLK in on %d)", LS_BOARD_ETH_PHY_ADDR,
                 LS_BOARD_ETH_MDC_GPIO, LS_BOARD_ETH_MDIO_GPIO,
                 LS_BOARD_ETH_REF_CLK_GPIO);
        return fail(e, "driver install");
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_ETH) == ESP_OK)
        esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, mac);
    esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac);

    /* lwIP joins 224.0.0.1 (01:00:5e:00:00:01) inside netif_add, and
       esp_netif (IDF 5.5.4, esp_netif_lwip_add) installs its multicast
       filter callback only after netif_add returns, so that join never
       reaches the EMAC. netif_remove's leave does, finds no such entry,
       and esp.emac, esp_eth, the glue and esp_netif_lwip each log an
       error on every stop. Installing the entry here makes the EMAC agree
       with what lwIP believes; the order of stop and destroy is not the
       cause, since the EMAC's filter calls do not check whether it is
       started. */
    static const uint8_t all_hosts[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    if (esp_eth_ioctl(s_eth, ETH_CMD_ADD_MAC_FILTER, (void *)all_hosts) != ESP_OK)
        ESP_LOGW(TAG, "all-hosts multicast filter not added; stop will log errors");

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = "ETH";
    base.if_desc = "eth";
    /* Above the Wi-Fi station's 100: esp_netif routes by this. */
    base.route_prio = LS_ETH_ROUTE_PRIO;
    const esp_netif_config_t ncfg = {
        .base = &base,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_netif = esp_netif_new(&ncfg);
    if (!s_netif) return fail(ESP_ERR_NO_MEM, "netif");
    s_glue = esp_eth_new_netif_glue(s_eth);
    if (!s_glue) return fail(ESP_ERR_NO_MEM, "netif glue");
    e = esp_netif_attach(s_netif, s_glue);
    if (e != ESP_OK) return fail(e, "netif attach");

    e = esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                            on_eth_event, NULL, &s_eth_evt);
    if (e == ESP_OK)
        e = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                on_ip_event, NULL, &s_got_ip_evt);
    if (e == ESP_OK)
        e = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                                on_ip_event, NULL, &s_lost_ip_evt);
    if (e != ESP_OK) return fail(e, "event handlers");

    e = esp_eth_start(s_eth);
    if (e != ESP_OK) return fail(e, "start");

    portENTER_CRITICAL(&s_lock);
    memcpy(s_st.mac, mac, sizeof(mac));
    s_st.started = true;
    s_st.start_err = ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "IP101 up at addr %d, MAC %02x:%02x:%02x:%02x:%02x:%02x, "
                  "waiting for link", LS_BOARD_ETH_PHY_ADDR,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t ls_eth_start(const ls_eth_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_setting = cfg->enabled;
    if (!cfg->enabled) {
        /* Off: a PHY in reset draws next to nothing and leaves its RMII
           inputs (GPIO35 is one) to whoever else wants them. */
        (void)phy_hold_reset();
        s_st.start_err = ESP_ERR_INVALID_STATE;
        ESP_LOGW(TAG, "off by setting ('eth on' or SET > DEVICE > Ethernet)");
        return ESP_ERR_INVALID_STATE;
    }
    if (!claim()) return ESP_ERR_INVALID_STATE;
    const esp_err_t e = bring_up();
    release();
    return e;
}

esp_err_t ls_eth_stop(void)
{
    if (!claim()) return ESP_ERR_INVALID_STATE;
    const bool was_up = s_eth || s_mac || s_phy;
    esp_err_t e = teardown();
    if (e == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_st.start_err = ESP_ERR_INVALID_STATE;
        portEXIT_CRITICAL(&s_lock);
        /* Only a pin the EMAC had is handed back; a stop with nothing
           running leaves the button as it already is. */
        if (was_up) {
            pin_hook(false);
            ESP_LOGW(TAG, "stopped - BOOT button back");
        }
    }
    release();
    return e;
}

bool ls_eth_enabled(void) { return s_setting; }

bool ls_eth_running(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool on = s_st.started;
    portEXIT_CRITICAL(&s_lock);
    return on;
}

esp_err_t ls_eth_set_enabled(bool on)
{
    esp_err_t e = ls_eth_setting_save(on);
    if (e != ESP_OK) return e;
    if (on) {
        if (!claim()) return ESP_ERR_INVALID_STATE;
        e = bring_up();
        release();
        return e;
    }
    return ls_eth_stop();
}

/* -------------------------------------------------------------- getters */

esp_netif_t *ls_eth_netif(void) { return s_netif; }

bool ls_eth_link_up(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool up = s_st.link_up;
    portEXIT_CRITICAL(&s_lock);
    return up;
}

bool ls_eth_has_ip(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool ip = s_st.has_ip;
    portEXIT_CRITICAL(&s_lock);
    return ip;
}

void ls_eth_status(ls_eth_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    portEXIT_CRITICAL(&s_lock);
    out->enabled = s_setting;
    /* Addresses straight from the netif rather than a copy kept from the
       event, so a lease renewal or a DNS server set elsewhere shows. */
    memset(&out->ip, 0, sizeof(out->ip));
    memset(&out->dns, 0, sizeof(out->dns));
    if (s_netif && out->has_ip) {
        esp_netif_get_ip_info(s_netif, &out->ip);
        esp_netif_dns_info_t dns = { 0 };
        if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4)
            out->dns = dns.ip.u_addr.ip4;
    }
}

/* -------------------------------------------------------------- setting */

static esp_err_t load_job(void *ctx)
{
    bool *en = ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;
    uint8_t v = 1;
    if (nvs_get_u8(h, NVS_KEY, &v) == ESP_OK) *en = v != 0;
    nvs_close(h);
    return ESP_OK;
}

bool ls_eth_setting_load(void)
{
    bool en = true;
    (void)ls_nvs_run(load_job, &en, 0);
    s_setting = en;
    return en;
}

static esp_err_t save_job(void *ctx)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, NVS_KEY, *(const bool *)ctx ? 1 : 0);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t ls_eth_setting_save(bool enabled)
{
    const esp_err_t e = ls_nvs_run(save_job, &enabled, 0);
    if (e == ESP_OK) s_setting = enabled;
    return e;
}
