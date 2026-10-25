/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. */
#pragma once

/* Which network interface carries the default route (and whose DNS servers
   lwIP uses) when more than one is up: Wi-Fi station and USB LTE today.

   The preference is "auto" (highest route_prio among the interfaces that
   have an address) or an esp_netif if_key such as "WIFI_STA_DEF" or "LTE":
   that interface when it has an address, else auto. */

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_NET_PREFER_MAX 16

/* Main task, at boot: the stored preference ("auto" when none). NVS. */
void ls_net_prefer_load(char *out, size_t cap);
/* Sets the running preference without storing it (boot). */
void ls_net_prefer_boot(const char *prefer);
/* Hooks the IP and Wi-Fi events. Needs the default event loop; safe to call
   more than once (each caller that creates one calls it). */
esp_err_t ls_net_init(void);
/* Console task only: stores the preference in NVS and applies it. */
esp_err_t ls_net_set_prefer(const char *prefer);
const char *ls_net_prefer(void);
/* Chooses and applies the default interface now. Any task but tcpip's. */
void ls_net_reevaluate(void);
/* An interface "up" for routing: link up and an IPv4 address. */
bool ls_net_netif_ready(esp_netif_t *n);

/* Every esp_netif, at most cap of them. */
int ls_net_list(esp_netif_t **out, int cap);
/* The DNS servers the interface was given (main, backup). false: none. */
bool ls_net_dns_of(esp_netif_t *n, esp_netif_dns_info_t d[2]);

void ls_net_console_register(void);

#ifdef __cplusplus
}
#endif
