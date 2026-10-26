/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. */

#include "ls_net.h"
#include "ls_net_policy.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi_types_generic.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "ls_nvs_safe.h"
#include "sdkconfig.h"

static const char *TAG = "ls_net";

#define NVS_NS  "lsnet"
#define NVS_KEY "prefer"
#define MAX_IFS 8

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_prefer[LS_NET_PREFER_MAX] = "auto";
static bool s_hooked;
static esp_timer_handle_t s_later;

/* DNS is kept per interface by esp_netif itself (main/Kconfig.projbuild
   selects ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF): each DHCP client files its
   servers under its own netif, in the tcpip thread, as the lease binds.
   Without it lwIP has one global list that the last interface to bind or
   renew overwrites, and a copy taken from an event handler later can be
   another interface's (an Ethernet LAN lease ended up holding the LTE
   carrier's 10.177.x resolvers that way). */
#if !CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF
#error "ls_net needs CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF"
#endif

/* The servers the interface itself was given; false when none known. */
static bool dns_of(esp_netif_t *n, esp_netif_dns_info_t d[2])
{
    memset(d, 0, 2 * sizeof(*d));
    esp_netif_get_dns_info(n, ESP_NETIF_DNS_MAIN, &d[0]);
    esp_netif_get_dns_info(n, ESP_NETIF_DNS_BACKUP, &d[1]);
    if (d[0].ip.type != ESP_IPADDR_TYPE_V4) d[0].ip.u_addr.ip4.addr = 0;
    if (d[1].ip.type != ESP_IPADDR_TYPE_V4) d[1].ip.u_addr.ip4.addr = 0;
    return d[0].ip.u_addr.ip4.addr != 0 || d[1].ip.u_addr.ip4.addr != 0;
}

bool ls_net_dns_of(esp_netif_t *n, esp_netif_dns_info_t d[2])
{
    return dns_of(n, d);
}

bool ls_net_netif_ready(esp_netif_t *n)
{
    esp_netif_ip_info_t ip;
    return n && esp_netif_is_netif_up(n) && esp_netif_get_ip_info(n, &ip) == ESP_OK &&
           ip.ip.addr != 0;
}

typedef struct { esp_netif_t *n[MAX_IFS]; int count; } if_list_t;

/* Runs in the tcpip thread: collect only, no esp_netif calls that
   would post back to it. */
static bool collect(esp_netif_t *n, void *ctx)
{
    if_list_t *l = ctx;
    if (l->count < MAX_IFS) l->n[l->count++] = n;
    return false;
}

int ls_net_list(esp_netif_t **out, int cap)
{
    if_list_t l = {0};
    esp_netif_find_if(collect, &l);
    int k = l.count < cap ? l.count : cap;
    memcpy(out, l.n, k * sizeof(*out));
    return k;
}

const char *ls_net_prefer(void)
{
    return s_prefer;
}

static void push_dns(esp_netif_dns_type_t type, uint32_t addr)
{
    if (!addr) return;
    esp_netif_dns_info_t d = { .ip.type = ESP_IPADDR_TYPE_V4 };
    d.ip.u_addr.ip4.addr = addr;
    /* NULL: lwIP's list only. The interface's own record is left as its
       DHCP filed it, so a stand-in never looks like a server it was given. */
    esp_netif_set_dns_info(NULL, type, &d);
}

void ls_net_reevaluate(void)
{
    char prefer[LS_NET_PREFER_MAX];
    taskENTER_CRITICAL(&s_lock);
    memcpy(prefer, s_prefer, sizeof(prefer));
    taskEXIT_CRITICAL(&s_lock);

    if_list_t l = {0};
    esp_netif_find_if(collect, &l);
    ls_net_if_t ifs[MAX_IFS];
    for (int i = 0; i < l.count; i++) {
        esp_netif_t *n = l.n[i];
        esp_netif_ip_info_t ip = {0};
        esp_netif_dns_info_t d[2];
        ifs[i].key = esp_netif_get_ifkey(n);
        ifs[i].up = ls_net_netif_ready(n);
        ifs[i].prio = esp_netif_get_route_prio(n);
        esp_netif_get_ip_info(n, &ip);
        ifs[i].gw = ip.gw.addr;
        dns_of(n, d);
        ifs[i].dns[0] = d[0].ip.u_addr.ip4.addr;
        ifs[i].dns[1] = d[1].ip.u_addr.ip4.addr;
    }
    ls_net_choice_t c;
    ls_net_choose(ifs, l.count, prefer, &c);
    /* Nothing up: leave lwIP's own choice alone. */
    if (c.index < 0) return;
    esp_netif_t *target = l.n[c.index];
    const bool moved = target != esp_netif_get_default_netif();
    /* Every time, not only on a change: setting the default copies this
       interface's own servers (empty slots included) into lwIP's list, which
       undoes a lease on another interface writing it meanwhile. Pinning
       also stops esp_netif's own route_prio switching, which would undo a
       preference on the next GOT_IP; that is why every IP event comes back
       through here. */
    esp_netif_set_default_netif(target);
    if (c.dns_from_gw) push_dns(ESP_NETIF_DNS_MAIN, c.dns[0]);
    else if (!ifs[c.index].dns[0]) push_dns(ESP_NETIF_DNS_MAIN, c.dns[0]);
    if (moved || c.dns_from_gw)
        ESP_LOGW(TAG, "default route: %s (%s)%s", ifs[c.index].key,
                 c.preferred ? "preferred" : "highest priority up",
                 c.dns_from_gw ? ", no DNS from its DHCP: using the gateway" : "");
}

static void later_cb(void *arg)
{
    ls_net_reevaluate();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        /* Wi-Fi's own handler takes the interface down after this one may
           have run; look again once it has. */
        if (id == WIFI_EVENT_STA_DISCONNECTED && s_later) {
            esp_timer_stop(s_later);
            esp_timer_start_once(s_later, 300 * 1000);
        }
        return;
    }
    switch (id) {
    case IP_EVENT_STA_GOT_IP:
    case IP_EVENT_ETH_GOT_IP:
    case IP_EVENT_PPP_GOT_IP:
        ls_net_reevaluate();
        break;
    case IP_EVENT_STA_LOST_IP:
    case IP_EVENT_ETH_LOST_IP:
    case IP_EVENT_PPP_LOST_IP:
        ls_net_reevaluate();
        break;
    default:
        break;
    }
}

esp_err_t ls_net_init(void)
{
    if (s_hooked) return ESP_OK;
    if (!s_later) {
        const esp_timer_create_args_t a = { .callback = later_cb, .name = "ls_net" };
        if (esp_timer_create(&a, &s_later) != ESP_OK) s_later = NULL;
    }
    esp_err_t e = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    if (e != ESP_OK) return e;
    e = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    if (e != ESP_OK) {
        esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, on_event);
        return e;
    }
    s_hooked = true;
    ls_net_reevaluate();
    return ESP_OK;
}

void ls_net_prefer_boot(const char *prefer)
{
    if (!prefer || !*prefer) prefer = "auto";
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_prefer, prefer, sizeof(s_prefer));
    taskEXIT_CRITICAL(&s_lock);
}

typedef struct { char *buf; size_t cap; } load_ctx_t;

static esp_err_t load_job(void *ctx)
{
    load_ctx_t *c = ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NOT_FOUND;
    size_t n = c->cap;
    esp_err_t e = nvs_get_str(h, NVS_KEY, c->buf, &n);
    nvs_close(h);
    return e;
}

void ls_net_prefer_load(char *out, size_t cap)
{
    load_ctx_t c = { out, cap };
    if (ls_nvs_run(load_job, &c, 0) != ESP_OK || !*out) strlcpy(out, "auto", cap);
}

static esp_err_t save_job(void *ctx)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, NVS_KEY, (const char *)ctx);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t ls_net_set_prefer(const char *prefer)
{
    char p[LS_NET_PREFER_MAX];
    if (!prefer || !*prefer || strlen(prefer) >= sizeof(p)) return ESP_ERR_INVALID_ARG;
    strlcpy(p, prefer, sizeof(p));
    esp_err_t e = ls_nvs_run(save_job, p, 0);
    ls_net_prefer_boot(p);
    ls_net_reevaluate();
    return e;
}
