/* `eth` console command.  See ls_eth.h. */
#include "ls_eth.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void print_status(void)
{
    ls_eth_status_t st;
    ls_eth_status(&st);

    /* A failed start hands GPIO35 back, so the button follows `started`. */
    printf("setting: %s (BOOT button %s)\n", st.enabled ? "on" : "off",
           st.started ? "given to RMII TXD1" : "available");
    if (!st.started) {
        printf("state:   not running (%s)\n",
               st.enabled ? esp_err_to_name(st.start_err) : "off by setting");
        return;
    }
    printf("MAC:     %02x:%02x:%02x:%02x:%02x:%02x  PHY IP101 at addr %u\n",
           st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5],
           (unsigned)st.phy_addr);
    if (st.link_up) {
        const int64_t up_s = (esp_timer_get_time() - st.link_since_us) / 1000000;
        printf("link:    up %d Mbps %s duplex, for %lldh%02lldm%02llds\n",
               st.speed_mbps, st.full_duplex ? "full" : "half",
               up_s / 3600, (up_s / 60) % 60, up_s % 60);
    } else {
        printf("link:    down (no cable, or nothing at the far end)\n");
    }
    printf("changes: %lu link up/down since boot\n", (unsigned long)st.link_changes);
    if (st.has_ip) {
        printf("IP:      " IPSTR " mask " IPSTR " gw " IPSTR "\n",
               IP2STR(&st.ip.ip), IP2STR(&st.ip.netmask), IP2STR(&st.ip.gw));
        printf("DNS:     " IPSTR "\n", IP2STR(&st.dns));
    } else {
        printf("IP:      none%s\n", st.link_up ? " yet (DHCP)" : "");
    }
    printf("route:   prio %d (Wi-Fi station 100)%s\n", LS_ETH_ROUTE_PRIO,
           esp_netif_get_default_netif() == ls_eth_netif() ? ", default route" : "");
    TaskHandle_t rx = xTaskGetHandle("emac_rx");
    if (rx)
        printf("emac_rx: %u bytes of stack never used\n",
               (unsigned)uxTaskGetStackHighWaterMark(rx));
}

int ls_eth_command(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) {
        print_status();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "phy")) {
        ls_eth_phy_dump();
        return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        const bool on = !strcmp(argv[1], "on");
        const esp_err_t e = ls_eth_set_enabled(on);
        if (e == ESP_OK) {
            printf(on ? "eth: on - GPIO35 is RMII TXD1, BOOT button off\n"
                      : "eth: off - PHY in reset, BOOT button on GPIO35\n");
            return 0;
        }
        if (ls_eth_enabled() != on)
            printf("eth: could not save the setting: %s\n", esp_err_to_name(e));
        else if (on)
            printf("eth: saved as on, but start failed: %s - BOOT button kept "
                   "(a reboot retries)\n", esp_err_to_name(e));
        else
            printf("eth: saved as off, but stop failed: %s - reboot to apply\n",
                   esp_err_to_name(e));
        return 1;
    }
    printf("eth [status] | eth phy | eth on|off   (on takes the BOOT button: GPIO35 is RMII TXD1)\n");
    return 1;
}
