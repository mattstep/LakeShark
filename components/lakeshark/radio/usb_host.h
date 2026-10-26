/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */

#ifndef USB_HOST_H
#define USB_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usb_boot_retry.h"
#include "usb_port_cycle.h"

#ifdef __cplusplus
extern "C" {
#endif

void class_driver_task(void *arg);
void class_driver_client_deregister(void);

/* Root-port power cycle through the IDF host library. See
   usb_port_cycle.c for the order it keeps and the race it cannot close. The
   caller must already have closed every handle it holds on the device. */
bool usb_host_root_port_has_device(void);
/* usb_port_attach_t: whether anything is plugged into the root port, whether
   or not it enumerated. UNKNOWN when it cannot be told. */
int usb_host_root_port_attached(void);
usb_port_cycle_result_t usb_host_root_port_cycle(void);
/* Print every enumerated device's descriptors, every configuration
   included. Opens and closes only. Returns the device count, -1 when the
   host is not up. */
int usb_host_dump_devices(void);
/* Send a mass-storage CBW (a Huawei mode switch) to the first Huawei
   mass-storage device, then watch it re-enumerate. -1: host not up. */
int usb_host_modeswitch(const uint8_t *cbw, size_t len);
/* One AT command on bulk interface itf of a Huawei modem; prints the
   reply. -1: host not up. */
int usb_host_at(int itf, const char *cmd);

#ifdef __cplusplus
}
#endif

#endif
