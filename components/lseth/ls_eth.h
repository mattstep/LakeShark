/* Wired Ethernet: the board's PHY on the P4's internal EMAC, as one more
   esp_netif beside Wi-Fi.  Built only with CONFIG_LS_ETH.

   Routing is left to esp_netif: this interface carries route_prio
   LS_ETH_ROUTE_PRIO, above the Wi-Fi station's 100, so a wired link with an
   address becomes the default route and Wi-Fi takes over again when it
   drops.  Choosing between links beyond that is not this module's job. */

#ifndef LS_ETH_H
#define LS_ETH_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_ETH_ROUTE_PRIO 120

typedef struct {
    /* The stored setting.  Off holds the PHY in reset and leaves the data
       pins (GPIO35 among them) alone. */
    bool enabled;
} ls_eth_config_t;

typedef struct {
    bool     enabled;        /* the stored setting */
    bool     started;        /* driver installed and running */
    bool     link_up;
    bool     has_ip;
    int      speed_mbps;     /* 10 / 100, 0 while down */
    bool     full_duplex;
    uint8_t  mac[6];
    uint32_t phy_addr;
    esp_netif_ip_info_t ip;  /* zero until DHCP answers */
    esp_ip4_addr_t dns;
    uint32_t link_changes;   /* up and down transitions since boot */
    int64_t  link_since_us;  /* esp_timer time of the last link-up, 0 if down */
    esp_err_t start_err;     /* why start failed, ESP_OK otherwise */
} ls_eth_status_t;

/* Called with true just before the EMAC takes the pin it shares with
   another user (GPIO35, the BOOT button), and with false once it has given
   it back.  With true it must not return until the other user has let go.
   Runs on whichever task starts or stops Ethernet. */
typedef void (*ls_eth_pin_hook_t)(bool eth_owns);
void ls_eth_set_pin_hook(ls_eth_pin_hook_t fn);

/* Boot: release the PHY from reset, bring up EMAC + PHY, attach a DHCP
   netif and start.  Brings up the default event loop and esp_netif if
   nothing has yet.  A PHY that does not answer is logged once, everything
   is torn down and the pin hook gets the pin back; the return says why.
   cfg->enabled false returns ESP_ERR_INVALID_STATE having done nothing but
   hold the PHY in reset. */
esp_err_t ls_eth_start(const ls_eth_config_t *cfg);

/* Take it all down again and hold the PHY in reset; the pin hook gets the
   pin back.  Idempotent, and safe after a start that failed.
   ESP_ERR_INVALID_STATE if another switch is in progress, or if the driver
   would not uninstall (then it stays half down until reboot). */
esp_err_t ls_eth_stop(void);

/* The netif pointer is valid until ls_eth_stop; NULL while stopped. */
esp_netif_t *ls_eth_netif(void);
bool         ls_eth_link_up(void);
bool         ls_eth_has_ip(void);
bool         ls_eth_running(void);
void         ls_eth_status(ls_eth_status_t *out);

/* The on/off setting (NVS "lseth"/"en", default on), shared by `eth on|off`
   and SET > DEVICE > Ethernet.  Load once from the boot task.  set_enabled
   saves it and applies it now: on starts Ethernet, off stops it.  It blocks
   the caller for the PHY reset and driver install (tens of ms) and returns
   the start or stop error; the setting is saved either way.  NVS goes
   through ls_nvs_run, so any task may call these. */
bool      ls_eth_setting_load(void);
esp_err_t ls_eth_setting_save(bool enabled);
bool      ls_eth_enabled(void);
esp_err_t ls_eth_set_enabled(bool on);

/* `eth` console command. */
int ls_eth_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* LS_ETH_H */
