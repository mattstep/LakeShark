/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. */
#pragma once

/* The route and DNS choice of ls_net, without esp_netif: given what each
   interface reports, which one carries the default route and which DNS
   servers lwIP should then use. Pure; the bench tests it on the host. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;  /* esp_netif if_key */
    bool up;          /* link up and an IPv4 address */
    int prio;         /* route_prio */
    uint32_t gw;      /* IPv4, lwIP order; 0 none */
    uint32_t dns[2];  /* the servers this interface itself was given */
} ls_net_if_t;

typedef struct {
    int index;        /* into the list; -1 when nothing is up */
    bool preferred;   /* the preference matched, not just priority */
    uint32_t dns[2];  /* for lwIP: the chosen interface's own servers */
    bool dns_from_gw; /* it had none, so its gateway stands in */
} ls_net_choice_t;

/* prefer: "auto" or an if_key. The preferred interface when it is up, else
   the up one with the highest priority (the first of equals). DNS is only
   ever the chosen interface's own: never another interface's. */
void ls_net_choose(const ls_net_if_t *ifs, int n, const char *prefer, ls_net_choice_t *out);

#ifdef __cplusplus
}
#endif
