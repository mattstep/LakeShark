/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */

#include "usb_host.h"
#include "usb_boot_retry.h"
#include "rtl_adapter_private.h"
#include "hackrf_adapter_private.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hal/usb_dwc_ll.h"
#include "esp_heap_caps.h"
#include "usb/usb_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "usb_host";

#define CLIENT_NUM_EVENT_MSG 5
#define DEV_MAX_COUNT        128

typedef enum {
    ACTION_OPEN_DEV        = (1 << 0),
    ACTION_GET_DEV_INFO    = (1 << 1),
    ACTION_GET_DEV_DESC    = (1 << 2),
    ACTION_GET_CONFIG_DESC = (1 << 3),
    ACTION_GET_STR_DESC    = (1 << 4),
    ACTION_CLOSE_DEV       = (1 << 5),
    ACTION_PROBE_DEV       = (1 << 6),
} action_t;

typedef struct {
    usb_host_client_handle_t client_hdl;
    uint8_t                  dev_addr;
    usb_device_handle_t      dev_hdl;
    action_t                 actions;
} usb_device_t;

typedef struct {
    struct {
        union {
            struct {
                uint8_t unhandled_devices : 1;
                uint8_t shutdown          : 1;
                uint8_t reserved6         : 6;
            };
            uint8_t val;
        } flags;
        usb_device_t device[DEV_MAX_COUNT];
    } mux_protected;
    struct {
        usb_host_client_handle_t client_hdl;
        SemaphoreHandle_t        mux_lock;
    } constant;
} class_driver_t;


static class_driver_t *s_driver_obj;
/* Devices that reached this client. A device that fails enumeration is
   still counted by usb_host_lib_info, but never arrives here. */
static volatile int s_client_devices;
/* The root port is unpowered by us (boot recovery, a port cycle). Its
   connect status reads empty then whatever is plugged in. */
static volatile bool s_root_port_off;
static esp_err_t root_port_set_power(bool on);

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    class_driver_t *driver_obj = (class_driver_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        s_client_devices++;
        xSemaphoreTake(driver_obj->constant.mux_lock, portMAX_DELAY);
        driver_obj->mux_protected.device[event_msg->new_dev.address].dev_addr =
            event_msg->new_dev.address;
        event_bus_publish_simple(EVT_DEVICE_ATTACHED, "usb");
        driver_obj->mux_protected.device[event_msg->new_dev.address].actions =
            ACTION_PROBE_DEV;
        driver_obj->mux_protected.flags.unhandled_devices = 1;
        xSemaphoreGive(driver_obj->constant.mux_lock);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        /**/
        (void)rtl_adapter_note_removed(event_msg->dev_gone.dev_hdl);
        (void)hackrf_adapter_note_removed(event_msg->dev_gone.dev_hdl);
        xSemaphoreTake(driver_obj->constant.mux_lock, portMAX_DELAY);
        for (uint8_t i = 0; i < DEV_MAX_COUNT; i++) {
            if (driver_obj->mux_protected.device[i].dev_hdl ==
                event_msg->dev_gone.dev_hdl) {
                driver_obj->mux_protected.device[i].actions = ACTION_CLOSE_DEV;
                driver_obj->mux_protected.flags.unhandled_devices = 1;
            }
        }
        event_bus_publish_simple(EVT_DEVICE_DETACHED, "usb");
        xSemaphoreGive(driver_obj->constant.mux_lock);
        break;
    /**/
    default:
        ESP_LOGW(TAG, "unhandled USB client event %d", (int)event_msg->event);
        break;
    }
}

/**/
static void action_open_dev(usb_device_t *d)
{
    if (d->dev_addr == 0) return;
    esp_err_t e = usb_host_device_open(d->client_hdl, d->dev_addr, &d->dev_hdl);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "device_open(addr=%u) failed: %s - dropping it",
                 d->dev_addr, esp_err_to_name(e));
        d->dev_hdl = NULL; d->dev_addr = 0;
        return;
    }
    d->actions |= ACTION_GET_DEV_INFO;
}
static void action_get_info(usb_device_t *d)
{
    usb_device_info_t i;
    if (!d->dev_hdl || usb_host_device_info(d->dev_hdl, &i) != ESP_OK) return;
    const char *sp = (i.speed == USB_SPEED_HIGH) ? "HIGH(480M)"
                   : (i.speed == USB_SPEED_FULL) ? "FULL(12M)"
                   : (i.speed == USB_SPEED_LOW)  ? "LOW(1.5M)" : "?";
    ESP_LOGW(TAG, "*** USB negotiated speed: %s (enum=%d) ***", sp, (int)i.speed);
    d->actions |= ACTION_GET_DEV_DESC;
}
static void action_get_dev_desc(usb_device_t *d)
{
    const usb_device_desc_t *dd;
    if (!d->dev_hdl || usb_host_get_device_descriptor(d->dev_hdl, &dd) != ESP_OK) return;
    printf("--- addr %u device descriptor ---\n", d->dev_addr);
    usb_print_device_descriptor(dd);
    d->actions |= ACTION_GET_CONFIG_DESC;
}
/* What usb_print_config_descriptor does not decode: class-specific
   descriptors (CDC functional ones are type 0x24 CS_INTERFACE, 0x25
   CS_ENDPOINT, subtype in byte 2). Printed raw. */
static void print_class_desc(const usb_standard_desc_t *s)
{
    const uint8_t *b = (const uint8_t *)s;
    printf("\t*** class-specific len %u type 0x%02x", b[0], b[1]);
    if (b[0] >= 3) printf(" subtype 0x%02x", b[2]);
    printf(":");
    for (int i = 0; i < b[0]; i++) printf(" %02x", b[i]);
    printf("\n");
}

static void action_get_config_desc(usb_device_t *d)
{
    const usb_config_desc_t *cd;
    if (!d->dev_hdl ||
        usb_host_get_active_config_descriptor(d->dev_hdl, &cd) != ESP_OK) return;
    printf("--- addr %u active configuration %u ---\n", d->dev_addr,
           cd->bConfigurationValue);
    usb_print_config_descriptor(cd, print_class_desc);
    d->actions |= ACTION_GET_STR_DESC;
}
static void action_get_str_desc(usb_device_t *d)
{
    usb_device_info_t i;
    if (!d->dev_hdl || usb_host_device_info(d->dev_hdl, &i) != ESP_OK) return;
    printf("--- addr %u strings ---\n", d->dev_addr);
    const usb_str_desc_t *str[] = { i.str_desc_manufacturer, i.str_desc_product,
                                    i.str_desc_serial_num };
    const char *name[] = { "manufacturer", "product", "serial" };
    for (int k = 0; k < 3; k++) {
        printf("%s: ", name[k]);
        if (str[k]) usb_print_string_descriptor(str[k]);
        else printf("(none)\n");
    }
}
static void action_close_dev(usb_device_t *d)
{
    if (d->dev_hdl) {
        esp_err_t e = usb_host_device_close(d->client_hdl, d->dev_hdl);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "device_close failed: %s", esp_err_to_name(e));
        }
    }
    d->dev_hdl = NULL; d->dev_addr = 0;
}

static void action_probe_dev(usb_device_t *d)
{
    usb_device_handle_t device = NULL;
    esp_err_t error = usb_host_device_open(d->client_hdl, d->dev_addr, &device);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "probe open(addr=%u): %s", d->dev_addr, esp_err_to_name(error));
        return;
    }
    const usb_device_desc_t *descriptor = NULL;
    error = usb_host_get_device_descriptor(device, &descriptor);
    bool valid = error == ESP_OK && descriptor;
    bool hackrf = valid && hackrf_adapter_matches(descriptor->idVendor,
                                                   descriptor->idProduct);
    /* Both adapters share this USB client. Release the descriptor probe before
     * starting exactly one adapter; overlapping opens return INVALID_STATE. */
    error = usb_host_device_close(d->client_hdl, device);
    if (error != ESP_OK || !valid) {
        ESP_LOGW(TAG, "USB descriptor probe could not hand off addr=%u", d->dev_addr);
        return;
    }
    if (hackrf) hackrf_adapter_probe_async(d->dev_addr, d->client_hdl);
    else rtl_adapter_probe_async(d->dev_addr, d->client_hdl);
}

static void device_handle(usb_device_t *d)
{
    uint8_t actions = d->actions;
    d->actions = 0;
    while (actions) {
        if (actions & ACTION_OPEN_DEV)        action_open_dev(d);
        if (actions & ACTION_GET_DEV_INFO)    action_get_info(d);
        if (actions & ACTION_GET_DEV_DESC)    action_get_dev_desc(d);
        if (actions & ACTION_GET_CONFIG_DESC) action_get_config_desc(d);
        if (actions & ACTION_GET_STR_DESC)    action_get_str_desc(d);
        if (actions & ACTION_CLOSE_DEV)       action_close_dev(d);
        if (actions & ACTION_PROBE_DEV)       action_probe_dev(d);
        actions = d->actions; d->actions = 0;
    }
}

void class_driver_task(void *arg)
{
    class_driver_t           obj = {0};
    usb_host_client_handle_t hdl = NULL;

    SemaphoreHandle_t mux = xSemaphoreCreateMutex();
    if (!mux) { ESP_LOGE(TAG, "mutex fail"); vTaskDelete(NULL); return; }

    usb_host_client_config_t cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
        .async = { .client_event_callback = client_event_cb,
                   .callback_arg          = (void *)&obj },
    };
    /* Registering allocates from internal RAM, which boot can run short of.
       A failure costs the USB radio, not the unit: retry for a while, then
       say so and stop, instead of aborting into a crash loop. */
    esp_err_t reg = ESP_FAIL;
    for (int i = 0; i < 10; i++) {
        reg = usb_host_client_register(&cfg, &hdl);
        if (reg == ESP_OK) break;
        ESP_LOGW(TAG, "client register failed: %s - retrying", esp_err_to_name(reg));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (reg != ESP_OK) {
        ESP_LOGE(TAG, "client register failed: %s - USB radios unavailable until reboot",
                 esp_err_to_name(reg));
        vSemaphoreDelete(mux);
        vTaskDelete(NULL);
        return;
    }

    obj.constant.mux_lock   = mux;
    obj.constant.client_hdl = hdl;
    for (uint8_t i = 0; i < DEV_MAX_COUNT; i++)
        obj.mux_protected.device[i].client_hdl = hdl;
    s_driver_obj = &obj;
    usb_boot_retry_t boot_retry={.next_us=esp_timer_get_time()+10000000};

    while (1) {
        if(!boot_retry.done) {
            const int64_t now=esp_timer_get_time();
            /* The port is only read once the retry is due to act on it. */
            const int attached=now>=boot_retry.next_us && !boot_retry.off
                ? usb_host_root_port_attached() : USB_PORT_ATTACH_UNKNOWN;
            usb_boot_retry_tick(&boot_retry,now,s_client_devices,attached,
                                root_port_set_power);
        }
        if (obj.mux_protected.flags.unhandled_devices) {
            xSemaphoreTake(obj.constant.mux_lock, portMAX_DELAY);
            for (uint8_t i = 0; i < DEV_MAX_COUNT; i++)
                if (obj.mux_protected.device[i].actions)
                    device_handle(&obj.mux_protected.device[i]);
            obj.mux_protected.flags.unhandled_devices = 0;
            xSemaphoreGive(obj.constant.mux_lock);
        } else {
            if (!obj.mux_protected.flags.shutdown)
                usb_host_client_handle_events(hdl, boot_retry.done ? portMAX_DELAY : pdMS_TO_TICKS(250));
            else break;
        }
    }

    ESP_ERROR_CHECK(usb_host_client_deregister(hdl));
    if (mux) vSemaphoreDelete(mux);
    vTaskSuspend(NULL);
}

void class_driver_client_deregister(void)
{
    if (!s_driver_obj) return;
    xSemaphoreTake(s_driver_obj->constant.mux_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < DEV_MAX_COUNT; i++) {
        if (s_driver_obj->mux_protected.device[i].dev_hdl != NULL) {
            s_driver_obj->mux_protected.device[i].actions |= ACTION_CLOSE_DEV;
            s_driver_obj->mux_protected.flags.unhandled_devices = 1;
        }
    }
    s_driver_obj->mux_protected.flags.shutdown = 1;
    xSemaphoreGive(s_driver_obj->constant.mux_lock);
    ESP_ERROR_CHECK(usb_host_client_unblock(s_driver_obj->constant.client_hdl));
}

/* 'usb' console command. The dump runs on the class driver task through
   the same open/describe/close actions; it never claims an interface, so
   a radio adapter is not disturbed (a device an adapter holds open fails
   the open and is skipped). Configurations other than the active one are
   read afterwards from the caller's task, since usb_host_get_config_desc
   blocks until the class driver task delivers its transfer. */
int usb_host_dump_devices(void)
{
    if (!s_driver_obj) return -1;
    class_driver_t *o = s_driver_obj;
    uint8_t addr[16];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(addr), addr, &n) != ESP_OK) return -1;
    xSemaphoreTake(o->constant.mux_lock, portMAX_DELAY);
    for (int k = 0; k < n; k++) {
        usb_device_t *d = &o->mux_protected.device[addr[k]];
        d->dev_addr = addr[k];
        d->actions |= ACTION_OPEN_DEV | ACTION_GET_DEV_INFO | ACTION_GET_DEV_DESC |
                      ACTION_GET_CONFIG_DESC | ACTION_GET_STR_DESC | ACTION_CLOSE_DEV;
    }
    if (n) o->mux_protected.flags.unhandled_devices = 1;
    xSemaphoreGive(o->constant.mux_lock);
    usb_host_client_unblock(o->constant.client_hdl);
    for (int t = 0; t < 100 && o->mux_protected.flags.unhandled_devices; t++)
        vTaskDelay(pdMS_TO_TICKS(20));

    for (int k = 0; k < n; k++) {
        usb_device_handle_t h;
        const usb_device_desc_t *dd;
        const usb_config_desc_t *active;
        if (usb_host_device_open(o->constant.client_hdl, addr[k], &h) != ESP_OK) continue;
        if (usb_host_get_device_descriptor(h, &dd) == ESP_OK &&
            usb_host_get_active_config_descriptor(h, &active) == ESP_OK &&
            dd->bNumConfigurations > 1) {
            for (uint8_t v = 1; v <= dd->bNumConfigurations; v++) {
                const usb_config_desc_t *cd = NULL;
                esp_err_t e = usb_host_get_config_desc(o->constant.client_hdl, h, v, &cd);
                printf("--- addr %u configuration index %u ---\n", addr[k], v - 1);
                if (e != ESP_OK) { printf("read failed: %s\n", esp_err_to_name(e)); continue; }
                if (cd->bConfigurationValue == active->bConfigurationValue)
                    printf("(bConfigurationValue %u, the active one, above)\n",
                           cd->bConfigurationValue);
                else
                    usb_print_config_descriptor(cd, print_class_desc);
                usb_host_free_config_desc(cd);
            }
        }
        usb_host_device_close(o->constant.client_hdl, h);
    }
    return n;
}

/* 'usb switch': send a Bulk-Only CBW to a Huawei stick in its
   mass-storage mode, which makes it re-enumerate as a modem. Runs on the
   caller's task; the transfers complete through the class driver task's
   usb_host_client_handle_events. The stick usually drops off the bus as
   soon as it takes the command, so a missing CSW is the normal outcome. */
static void ms_xfer_done(usb_transfer_t *t)
{
    xSemaphoreGive((SemaphoreHandle_t)t->context);
}

/* Submit and wait. On timeout, halt and flush the endpoint so the transfer
   comes back cancelled. false: it never came back and must not be freed. */
static bool ms_xfer(usb_transfer_t *t, uint32_t ms, esp_err_t *err)
{
    SemaphoreHandle_t done = (SemaphoreHandle_t)t->context;
    *err = usb_host_transfer_submit(t);
    if (*err != ESP_OK) return true;
    if (xSemaphoreTake(done, pdMS_TO_TICKS(ms)) == pdTRUE) return true;
    *err = ESP_ERR_TIMEOUT;
    usb_host_endpoint_halt(t->device_handle, t->bEndpointAddress);
    usb_host_endpoint_flush(t->device_handle, t->bEndpointAddress);
    if (xSemaphoreTake(done, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
    return true;
}

static const char *xfer_status_name(usb_transfer_status_t st)
{
    switch (st) {
    case USB_TRANSFER_STATUS_COMPLETED: return "completed";
    case USB_TRANSFER_STATUS_ERROR:     return "error";
    case USB_TRANSFER_STATUS_TIMED_OUT: return "timed out";
    case USB_TRANSFER_STATUS_CANCELED:  return "cancelled";
    case USB_TRANSFER_STATUS_STALL:     return "stall";
    case USB_TRANSFER_STATUS_OVERFLOW:  return "overflow";
    case USB_TRANSFER_STATUS_SKIPPED:   return "skipped";
    case USB_TRANSFER_STATUS_NO_DEVICE: return "device gone";
    default:                            return "?";
    }
}

int usb_host_modeswitch(const uint8_t *cbw, size_t len)
{
    if (!s_driver_obj) return -1;
    usb_host_client_handle_t c = s_driver_obj->constant.client_hdl;
    uint8_t addr[16];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(addr), addr, &n) != ESP_OK) return -1;
    usb_device_handle_t h = NULL;
    uint8_t from = 0;
    uint16_t pid = 0;
    for (int k = 0; k < n && !h; k++) {
        const usb_device_desc_t *dd;
        const usb_config_desc_t *cd;
        if (usb_host_device_open(c, addr[k], &h) != ESP_OK) { h = NULL; continue; }
        int off = 0;
        const usb_intf_desc_t *in = NULL;
        if (usb_host_get_device_descriptor(h, &dd) == ESP_OK && dd->idVendor == 0x12d1 &&
            usb_host_get_active_config_descriptor(h, &cd) == ESP_OK)
            in = usb_parse_interface_descriptor(cd, 0, 0, &off);
        if (in && in->bInterfaceClass == USB_CLASS_MASS_STORAGE) {
            from = addr[k];
            pid = dd->idProduct;
        } else {
            usb_host_device_close(c, h);
            h = NULL;
        }
    }
    if (!h) { printf("no Huawei mass-storage device on the port\n"); return 0; }
    printf("addr %u 12d1:%04x: sending %u-byte CBW on EP 0x01\n", from, pid, (unsigned)len);

    usb_transfer_t *t = NULL;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    esp_err_t e = usb_host_interface_claim(c, h, 0, 0);
    bool claimed = e == ESP_OK, freeable = true;
    if (!claimed) printf("interface claim: %s\n", esp_err_to_name(e));
    else if (!done || usb_host_transfer_alloc(512, 0, &t) != ESP_OK) {
        printf("out of memory\n");
        t = NULL;
    } else {
        t->device_handle = h;
        t->callback = ms_xfer_done;
        t->context = done;
        t->timeout_ms = 0;
        memcpy(t->data_buffer, cbw, len);
        t->num_bytes = len;
        t->bEndpointAddress = 0x01;
        freeable = ms_xfer(t, 1000, &e);
        if (e != ESP_OK) printf("CBW: %s\n", esp_err_to_name(e));
        else printf("CBW: %s, %d bytes\n", xfer_status_name(t->status), t->actual_num_bytes);
        if (freeable && e == ESP_OK && t->status == USB_TRANSFER_STATUS_COMPLETED) {
            t->num_bytes = 512;
            t->bEndpointAddress = 0x81;
            freeable = ms_xfer(t, 1000, &e);
            if (e != ESP_OK) printf("CSW: %s\n", esp_err_to_name(e));
            else if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes >= 13)
                printf("CSW: tag %02x%02x%02x%02x residue %lu status %u\n",
                       t->data_buffer[4], t->data_buffer[5], t->data_buffer[6],
                       t->data_buffer[7],
                       (unsigned long)(t->data_buffer[8] | t->data_buffer[9] << 8 |
                                       t->data_buffer[10] << 16 |
                                       (uint32_t)t->data_buffer[11] << 24),
                       t->data_buffer[12]);
            else printf("CSW: %s, %d bytes\n", xfer_status_name(t->status),
                        t->actual_num_bytes);
        }
    }
    if (claimed) {
        e = usb_host_interface_release(c, h, 0);
        if (e != ESP_OK) printf("interface release: %s\n", esp_err_to_name(e));
    }
    e = usb_host_device_close(c, h);
    if (e != ESP_OK) printf("device close: %s\n", esp_err_to_name(e));
    if (t && freeable) usb_host_transfer_free(t);
    else if (t) printf("transfer never came back; leaking it\n");
    if (done && freeable) vSemaphoreDelete(done);

    /* Watch for the stick to leave and come back as something else. */
    bool gone = false;
    for (int ms = 0; ms <= 15000; ms += 250) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (usb_host_device_addr_list_fill(sizeof(addr), addr, &n) != ESP_OK) continue;
        bool still = false;
        for (int k = 0; k < n; k++) {
            usb_device_handle_t d;
            const usb_device_desc_t *dd;
            if (usb_host_device_open(c, addr[k], &d) != ESP_OK) continue;
            if (usb_host_get_device_descriptor(d, &dd) == ESP_OK) {
                if (addr[k] == from && dd->idProduct == pid) still = true;
                else {
                    printf("after %d ms: addr %u %04x:%04x\n", ms + 250, addr[k],
                           dd->idVendor, dd->idProduct);
                    usb_host_device_close(c, d);
                    return 1;
                }
            }
            usb_host_device_close(c, d);
        }
        if (!still && !gone) { printf("detached after %d ms\n", ms + 250); gone = true; }
    }
    printf(gone ? "did not come back within 15 s\n" : "still there, unchanged, after 15 s\n");
    return 1;
}

static bool at_final_line(const char *buf)
{
    static const struct { const char *s; bool prefix; } fin[] = {
        { "OK", false }, { "ERROR", false }, { "COMMAND NOT SUPPORT", false },
        { "NO CARRIER", false }, { "+CME ERROR", true }, { "+CMS ERROR", true },
    };
    for (const char *l = buf; *l; ) {
        const char *e = l + strcspn(l, "\r\n");
        const size_t n = (size_t)(e - l);
        for (size_t k = 0; k < sizeof(fin) / sizeof(fin[0]); k++) {
            const size_t f = strlen(fin[k].s);
            if ((fin[k].prefix ? n >= f : n == f) && !strncmp(l, fin[k].s, f)) return true;
        }
        l = *e ? e + 1 : e;
    }
    return false;
}

/* A newly claimed interface gets pipes that start at DATA0, while the
   device's bulk endpoints carry on from wherever the last session left
   their data toggle. A mismatched IN packet is taken for a retry and
   silently dropped. CLEAR_FEATURE(ENDPOINT_HALT) puts the device's side
   back to DATA0 too. */
static void ep_sync(usb_host_client_handle_t c, usb_device_handle_t h,
                    SemaphoreHandle_t done, uint8_t ep)
{
    usb_transfer_t *x;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &x) != ESP_OK) return;
    usb_setup_packet_t *sp = (usb_setup_packet_t *)x->data_buffer;
    sp->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                        USB_BM_REQUEST_TYPE_RECIP_ENDPOINT;
    sp->bRequest = USB_B_REQUEST_CLEAR_FEATURE;
    sp->wValue = 0; /* ENDPOINT_HALT */
    sp->wIndex = ep;
    sp->wLength = 0;
    x->num_bytes = sizeof(usb_setup_packet_t);
    x->device_handle = h;
    x->bEndpointAddress = 0;
    x->callback = ms_xfer_done;
    x->context = done;
    if (usb_host_transfer_submit_control(c, x) == ESP_OK &&
        xSemaphoreTake(done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        printf("CLEAR_FEATURE(0x%02x) never came back; leaking it\n", ep);
        return;
    }
    usb_host_transfer_free(x);
}

/* 'usb at': one AT command on a Huawei vendor serial port of a stick in
   modem mode. Endpoints come from the interface descriptor. A command that
   reboots the stick (^SETPORT) loses the device mid-way; that is reported,
   not fatal. */
int usb_host_at(int itf, const char *cmd)
{
    if (!s_driver_obj) return -1;
    usb_host_client_handle_t c = s_driver_obj->constant.client_hdl;
    uint8_t addr[16];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(addr), addr, &n) != ESP_OK) return -1;
    usb_device_handle_t h = NULL;
    uint8_t in_ep = 0, out_ep = 0;
    for (int k = 0; k < n && !h; k++) {
        const usb_device_desc_t *dd;
        const usb_config_desc_t *cd;
        if (usb_host_device_open(c, addr[k], &h) != ESP_OK) { h = NULL; continue; }
        int off = 0;
        const usb_intf_desc_t *in = NULL;
        if (usb_host_get_device_descriptor(h, &dd) == ESP_OK && dd->idVendor == 0x12d1 &&
            usb_host_get_active_config_descriptor(h, &cd) == ESP_OK)
            in = usb_parse_interface_descriptor(cd, itf, 0, &off);
        if (in && in->bInterfaceClass != USB_CLASS_MASS_STORAGE) {
            for (int i = 0; i < in->bNumEndpoints; i++) {
                int eo = off;
                const usb_ep_desc_t *ep =
                    usb_parse_endpoint_descriptor_by_index(in, i, cd->wTotalLength, &eo);
                if (!ep || (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) !=
                               USB_BM_ATTRIBUTES_XFER_BULK) continue;
                if (USB_EP_DESC_GET_EP_DIR(ep)) in_ep = ep->bEndpointAddress;
                else out_ep = ep->bEndpointAddress;
            }
        }
        if (!in_ep || !out_ep) {
            usb_host_device_close(c, h);
            h = NULL; in_ep = out_ep = 0;
        }
    }
    if (!h) { printf("no Huawei modem with bulk endpoints on interface %d\n", itf); return 0; }

    usb_transfer_t *t = NULL;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    esp_err_t e = usb_host_interface_claim(c, h, itf, 0);
    bool claimed = e == ESP_OK, freeable = true;
    size_t len = strlen(cmd);
    /* The LTE driver holds the AT port while it runs; 'lte at' goes
       through it instead. */
    if (!claimed) printf("interface %d claim: %s (in use? try 'lte at')\n", itf,
                         esp_err_to_name(e));
    else if (!done || len > 500 || usb_host_transfer_alloc(512, 0, &t) != ESP_OK) {
        printf("out of memory or command too long\n");
        t = NULL;
    } else {
        ep_sync(c, h, done, in_ep);
        ep_sync(c, h, done, out_ep);
        t->device_handle = h;
        t->callback = ms_xfer_done;
        t->context = done;
        memcpy(t->data_buffer, cmd, len);
        t->data_buffer[len++] = '\r';
        t->num_bytes = len;
        t->bEndpointAddress = out_ep;
        printf("itf %d (OUT 0x%02x IN 0x%02x) > %s\n", itf, out_ep, in_ep, cmd);
        freeable = ms_xfer(t, 1000, &e);
        if (freeable && (e != ESP_OK || t->status != USB_TRANSFER_STATUS_COMPLETED))
            printf("write: %s\n", e != ESP_OK ? esp_err_to_name(e)
                                             : xfer_status_name(t->status));
        else if (freeable) {
            /* PSRAM: internal RAM is the scarce heap on this board, and a
               console reply has no reason to live there. */
            enum { RESP_CAP = 2048 };
            char *resp = heap_caps_malloc(RESP_CAP, MALLOC_CAP_SPIRAM);
            if (!resp) resp = malloc(RESP_CAP);
            size_t got = 0;
            if (!resp) { printf("out of memory\n"); goto out; }
            resp[0] = 0;
            /* One IN transfer stays queued until the device sends a short
               packet; it is only cancelled when the time is up, because a
               cancel can drop a packet already on its way. */
            const int64_t end = esp_timer_get_time() + 3000000;
            for (int64_t now; (now = esp_timer_get_time()) < end; ) {
                t->num_bytes = 512;
                t->bEndpointAddress = in_ep;
                freeable = ms_xfer(t, (uint32_t)((end - now) / 1000) + 1, &e);
                if (!freeable) break;
                if (e == ESP_OK && t->status == USB_TRANSFER_STATUS_COMPLETED) {
                    size_t m = t->actual_num_bytes;
                    if (m > RESP_CAP - 1 - got) m = RESP_CAP - 1 - got;
                    memcpy(resp + got, t->data_buffer, m);
                    got += m;
                    resp[got] = 0;
                    if (at_final_line(resp)) break;
                } else if (e == ESP_OK && t->status == USB_TRANSFER_STATUS_NO_DEVICE) {
                    printf("(device gone)\n");
                    break;
                } else if (e != ESP_ERR_TIMEOUT) {
                    printf("read: %s\n", e != ESP_OK ? esp_err_to_name(e)
                                                     : xfer_status_name(t->status));
                    break;
                }
            }
            for (size_t i = 0; i < got; i++) {
                const char ch = resp[i];
                if (ch == '\r') continue;
                if (ch == '\n' || (ch >= 0x20 && ch < 0x7f)) putchar(ch);
                else printf("<%02x>", (uint8_t)ch);
            }
            printf("%s[%u bytes]\n", got && resp[got - 1] != '\n' ? "\n" : "", (unsigned)got);
            free(resp);
        }
    }
out:
    if (claimed) usb_host_interface_release(c, h, itf);
    usb_host_device_close(c, h);
    if (t && freeable) usb_host_transfer_free(t);
    else if (t) printf("transfer never came back; leaking it\n");
    if (done && freeable) vSemaphoreDelete(done);
    return 1;
}

static esp_err_t root_port_set_power(bool on)
{
    const esp_err_t err = usb_host_lib_set_root_port_power(on);
    if (err == ESP_OK) s_root_port_off = !on;
    return err;
}

static int root_port_device_count(void)
{
    usb_host_lib_info_t info;
    if (usb_host_lib_info(&info) != ESP_OK) return -1;
    return info.num_devices;
}

static void root_port_delay_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

/* Whether anything is plugged into the root port, enumerated or not. The
   host library cannot say: a device that fails before it is registered
   (a failed port reset) is not in usb_host_lib_info, and one that fails
   later still is. The controller's own port status (HPRT.PRTCONNSTS, the
   bit the IDF HCD raises its connection event from) is set by the plug
   alone. Read only: no write-1-to-clear bit is touched. HS controller,
   because the IDF host runs on port 0 and the lib is installed with the
   default peripheral map. UNKNOWN until the client exists (the host is
   installed) and while we hold the port unpowered, when it reads empty
   whatever is plugged in. */
int usb_host_root_port_attached(void)
{
    if (!s_driver_obj || s_root_port_off) return USB_PORT_ATTACH_UNKNOWN;
    if (root_port_device_count() > 0) return USB_PORT_ATTACH_PRESENT;
    return usb_dwc_ll_hprt_get_conn_status(USB_DWC_LL_GET_HW(0))
        ? USB_PORT_ATTACH_PRESENT : USB_PORT_ATTACH_NONE;
}

bool usb_host_root_port_has_device(void)
{
    return root_port_device_count() > 0;
}

/**/
usb_port_cycle_result_t usb_host_root_port_cycle(void)
{
    static const usb_port_cycle_ops_t ops = {
        .set_power = root_port_set_power,
        .device_count = root_port_device_count,
        .delay_ms = root_port_delay_ms,
    };
    return usb_port_cycle_run(&ops);
}
