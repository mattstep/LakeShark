/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. */

/* 'net': list the interfaces, choose the preferred one, and two probes
   (ping, one HTTP GET) that show which way traffic actually goes. */

#include "ls_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"

#define IP4(a) (unsigned)((a) & 0xff), (unsigned)((a) >> 8 & 0xff), (unsigned)((a) >> 16 & 0xff), (unsigned)((a) >> 24 & 0xff)

static void list(void)
{
    esp_netif_t *n[8];
    const int count = ls_net_list(n, 8);
    esp_netif_t *def = esp_netif_get_default_netif();
    printf("prefer: %s\n", ls_net_prefer());
    for (int i = 0; i < count; i++) {
        esp_netif_ip_info_t ip = {0};
        esp_netif_get_ip_info(n[i], &ip);
        const bool up = esp_netif_is_netif_up(n[i]);
        printf("%c %-13s %-5s prio %3d %-4s %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u",
               n[i] == def ? '*' : ' ', esp_netif_get_ifkey(n[i]), esp_netif_get_desc(n[i]),
               esp_netif_get_route_prio(n[i]), up ? "up" : "down", IP4(ip.ip.addr),
               IP4(ip.netmask.addr), IP4(ip.gw.addr));
        esp_netif_dns_info_t d[2];
        if (ls_net_dns_of(n[i], d)) {
            printf(" dns %u.%u.%u.%u", IP4(d[0].ip.u_addr.ip4.addr));
            if (d[1].ip.u_addr.ip4.addr) printf(",%u.%u.%u.%u", IP4(d[1].ip.u_addr.ip4.addr));
        }
        printf("\n");
    }
    esp_netif_dns_info_t g = {0};
    if (def && esp_netif_get_dns_info(NULL, ESP_NETIF_DNS_MAIN, &g) != ESP_OK)
        esp_netif_get_dns_info(def, ESP_NETIF_DNS_MAIN, &g);
    printf("resolver in use: %u.%u.%u.%u\n", IP4(g.ip.u_addr.ip4.addr));
}

/* Ping and HTTP run on a short-lived worker with a PSRAM stack: the
   console task's 4 KB internal stack is too small for the HTTP client and
   the resolver, and internal RAM too scarce to give it more. */
typedef struct {
    SemaphoreHandle_t done;
    SemaphoreHandle_t end; /* the ping session's own end */
    const char *arg;
    int count;
    int rc;
    uint32_t ok;
} job_t;

static bool resolve(const char *host, ip_addr_t *out)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_RAW };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    ip_addr_set_zero(out);
    IP_SET_TYPE(out, IPADDR_TYPE_V4);
    ip4_addr_set_u32(ip_2_ip4(out), sa->sin_addr.s_addr);
    freeaddrinfo(res);
    return true;
}

static void on_ping_ok(esp_ping_handle_t h, void *arg)
{
    uint16_t seq; uint8_t ttl; uint32_t ms, len;
    ip_addr_t from;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(h, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    esp_ping_get_profile(h, ESP_PING_PROF_SIZE, &len, sizeof(len));
    esp_ping_get_profile(h, ESP_PING_PROF_IPADDR, &from, sizeof(from));
    printf("%lu bytes from %s: icmp_seq=%u ttl=%u time=%lu ms\n", (unsigned long)len,
           ipaddr_ntoa(&from), seq, ttl, (unsigned long)ms);
}

static void on_ping_timeout(esp_ping_handle_t h, void *arg)
{
    uint16_t seq;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    printf("icmp_seq=%u timed out\n", seq);
}

static void on_ping_end(esp_ping_handle_t h, void *arg)
{
    job_t *j = arg;
    uint32_t sent, got, ms;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &got, sizeof(got));
    esp_ping_get_profile(h, ESP_PING_PROF_DURATION, &ms, sizeof(ms));
    /* DURATION is the sum of the round trips, not the wall time. */
    printf("%lu sent, %lu received, %lu%% loss, avg %lu ms\n", (unsigned long)sent,
           (unsigned long)got, sent ? (unsigned long)((sent - got) * 100 / sent) : 0UL,
           got ? (unsigned long)(ms / got) : 0UL);
    j->ok = got;
    xSemaphoreGive(j->end);
}

static void ping_job(job_t *j)
{
    esp_netif_t *def = esp_netif_get_default_netif();
    if (!def || !ls_net_netif_ready(def)) { printf("no interface is up\n"); j->rc = 1; return; }
    ip_addr_t target;
    if (!resolve(j->arg, &target)) { printf("cannot resolve %s\n", j->arg); j->rc = 1; return; }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = j->count;
    cfg.interval_ms = 1000;
    cfg.timeout_ms = 2000;
    /* Bound to the routed interface, so the reply says which way it went. */
    cfg.interface = esp_netif_get_netif_impl_index(def);
    printf("PING %s (%s) via %s\n", j->arg, ipaddr_ntoa(&target), esp_netif_get_ifkey(def));
    esp_ping_callbacks_t cbs = { .cb_args = j, .on_ping_success = on_ping_ok,
                                 .on_ping_timeout = on_ping_timeout, .on_ping_end = on_ping_end };
    esp_ping_handle_t ping;
    j->end = xSemaphoreCreateBinary();
    if (!j->end || esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        printf("ping session failed (memory?)\n");
        if (j->end) vSemaphoreDelete(j->end);
        j->rc = 1;
        return;
    }
    esp_ping_start(ping);
    xSemaphoreTake(j->end, pdMS_TO_TICKS((j->count + 2) * 3000));
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    vSemaphoreDelete(j->end);
    j->rc = j->ok ? 0 : 1;
}

static void http_job(job_t *j)
{
    esp_http_client_config_t cfg = {
        .url = j->arg,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { printf("bad URL or out of memory\n"); j->rc = 1; return; }
    esp_netif_t *def = esp_netif_get_default_netif();
    printf("GET %s via %s\n", j->arg, def ? esp_netif_get_ifkey(def) : "?");
    const int64_t t0 = esp_timer_get_time();
    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        printf("connect: %s\n", esp_err_to_name(e));
        esp_http_client_cleanup(c);
        j->rc = 1;
        return;
    }
    const int64_t clen = esp_http_client_fetch_headers(c);
    const int status = esp_http_client_get_status_code(c);
    char *buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    char head[201];
    size_t headn = 0;
    int64_t total = 0;
    for (int n; buf && (n = esp_http_client_read(c, buf, 1024)) > 0; ) {
        for (int i = 0; i < n && headn < 200; i++) head[headn++] = buf[i];
        total += n;
    }
    const int64_t us = esp_timer_get_time() - t0;
    head[headn] = 0;
    printf("HTTP %d, content-length %lld, read %lld bytes in %lld ms", status,
           (long long)clen, (long long)total, (long long)(us / 1000));
    if (us > 0 && total > 4096)
        printf(" (%lld kB/s)", (long long)(total * 1000 / us));
    printf("\n");
    /* Only text: a zip's first 200 bytes are noise on a serial console. */
    bool text = headn > 0;
    for (size_t i = 0; i < headn && text; i++)
        if ((head[i] < 0x20 || head[i] > 0x7e) && head[i] != '\n' && head[i] != '\r' &&
            head[i] != '\t') text = false;
    if (text) printf("%s%s", head, head[headn - 1] == '\n' ? "" : "\n");
    else if (headn) printf("(binary body)\n");
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    j->rc = status >= 200 && status < 400 ? 0 : 1;
}

static void ping_worker(void *arg) { job_t *j = arg; ping_job(j); xSemaphoreGive(j->done); vTaskSuspend(NULL); }
static void http_worker(void *arg) { job_t *j = arg; http_job(j); xSemaphoreGive(j->done); vTaskSuspend(NULL); }

/* Waits for the worker however long it takes: it uses the job on this
   stack, and both probes are bounded by their own timeouts. */
static int run_job(TaskFunction_t fn, const char *arg, int count)
{
    job_t j = { .done = xSemaphoreCreateBinary(), .arg = arg, .count = count, .rc = 1 };
    if (!j.done) { printf("out of memory\n"); return 1; }
    TaskHandle_t t = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(fn, "net_job", 6144, &j, 5, &t, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        printf("cannot start the worker (memory)\n");
        vSemaphoreDelete(j.done);
        return 1;
    }
    xSemaphoreTake(j.done, portMAX_DELAY);
    while (eTaskGetState(t) != eSuspended) vTaskDelay(1);
    vTaskDeleteWithCaps(t);
    vSemaphoreDelete(j.done);
    return j.rc;
}

static int cmd_net(int argc, char **argv)
{
    if (argc == 1) { list(); return 0; }
    if (!strcmp(argv[1], "prefer") && argc == 3) {
        const char *p = argv[2];
        if (!strcmp(p, "wifi")) p = "WIFI_STA_DEF";
        else if (!strcmp(p, "lte")) p = "LTE";
        esp_err_t e = ls_net_set_prefer(p);
        if (e != ESP_OK) printf("prefer %s: %s\n", p, esp_err_to_name(e));
        list();
        return e == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "ping") && (argc == 3 || argc == 4)) {
        int count = argc == 4 ? atoi(argv[3]) : 4;
        if (count < 1) count = 1;
        if (count > 500) count = 500;
        return run_job(ping_worker, argv[2], count);
    }
    if (!strcmp(argv[1], "http") && argc == 3)
        return run_job(http_worker, argv[2], 0);
    printf("usage: net [prefer auto|wifi|lte|<if_key> | ping <host> [count] | http <url>]\n");
    return 1;
}

void ls_net_console_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "net",
        .help = "Network interfaces: address, DNS, '*' the default route. 'net prefer "
                "auto|wifi|lte' picks the route (stored), 'net ping <host> [n]' pings over "
                "it, 'net http <url>' fetches one URL",
        .hint = "[prefer auto|wifi|lte | ping <host> [n] | http <url>]",
        .func = &cmd_net,
    };
    esp_console_cmd_register(&cmd);
}
