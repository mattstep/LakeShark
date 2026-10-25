/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. */

#include "ls_net_policy.h"

#include <string.h>

void ls_net_choose(const ls_net_if_t *ifs, int n, const char *prefer, ls_net_choice_t *out)
{
    memset(out, 0, sizeof(*out));
    out->index = -1;
    const bool any = !prefer || !*prefer || !strcmp(prefer, "auto");
    int best = -1, pref = -1;
    for (int i = 0; i < n; i++) {
        if (!ifs[i].up) continue;
        if (!any && pref < 0 && ifs[i].key && !strcmp(ifs[i].key, prefer)) pref = i;
        if (best < 0 || ifs[i].prio > ifs[best].prio) best = i;
    }
    const int c = pref >= 0 ? pref : best;
    if (c < 0) return;
    out->index = c;
    out->preferred = pref >= 0;
    if (ifs[c].dns[0]) {
        out->dns[0] = ifs[c].dns[0];
        out->dns[1] = ifs[c].dns[1];
    } else if (ifs[c].dns[1]) {
        out->dns[0] = ifs[c].dns[1];
    } else if (ifs[c].gw) {
        /* A LAN whose DHCP names no DNS server: home routers answer DNS
           on their own address far more often than not. */
        out->dns[0] = ifs[c].gw;
        out->dns_from_gw = true;
    }
}
