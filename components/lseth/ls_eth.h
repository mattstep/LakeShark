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
    /* The stored `eth on|off` setting.  Off holds the PHY in reset and
       leaves the data pins (GPIO35 among them) alone. */
    bool enabled;
} ls_eth_config_t;

typedef struct {
    bool     enabled;        /* the setting this boot started with */
    bool     started;        /* driver installed and running */
    bool     link_up;
    bool     has_ip;
    int      speed_mbps;     /* 10 / 100, 0 while down */
    bool     full_duplex;
    uint8_t  mac[6];
    uint32_t phy_addr;
    esp_netif_ip_info_t ip;  /* zero until DHCP answers */
    esp_ip4_addr_t dns;
    uint32_t link_changes;   /* up and down transitions since start */
    int64_t  link_since_us;  /* esp_timer time of the last link-up, 0 if down */
    esp_err_t start_err;     /* why start failed, ESP_OK otherwise */
} ls_eth_status_t;

/* Release the PHY from reset, bring up EMAC + PHY, attach a DHCP netif and
   start.  Call once, from the boot task, after NVS.  Brings up the default
   event loop and esp_netif if nothing has yet.  A PHY that does not answer
   is logged once and everything is torn down again; the return says why.
   cfg->enabled false returns ESP_ERR_INVALID_STATE having done nothing but
   hold the PHY in reset. */
esp_err_t ls_eth_start(const ls_eth_config_t *cfg);

esp_netif_t *ls_eth_netif(void);
bool         ls_eth_link_up(void);
bool         ls_eth_has_ip(void);
void         ls_eth_status(ls_eth_status_t *out);

/* The `eth on|off` setting (NVS "lseth"/"en", default on).  Load from the
   boot task, save from the console; both go through ls_nvs_run. */
bool      ls_eth_setting_load(void);
esp_err_t ls_eth_setting_save(bool enabled);

/* `eth` console command. */
int ls_eth_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* LS_ETH_H */
