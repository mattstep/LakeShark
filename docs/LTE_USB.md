# USB LTE stick

LakeShark can use a Huawei LTE USB stick on the T-Display-P4's USB-A host
port as a second network interface beside Wi-Fi. It was developed and tested
with a **Huawei MS2372h-517** (firmware 21.328.02.01.00) on T-Mobile US
(APN `h2g2`). Other Huawei sticks that expose the same vendor-class NCM
interface (Linux binds `huawei_cdc_ncm` to them) should work the same way.

Build it in with the overlay:

```
idf.py -B build_tdp4_lte -D SDKCONFIG=build_tdp4_lte/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults;boards/lte.defaults" build
```

`boards/lte.defaults` sets `CONFIG_LS_LTE=y` and
`CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF=y` (see Routing below). The
driver lives in `components/lslte`, the routing policy in `components/lsnet`.

## The two USB modes

A cold-plugged stick enumerates as **12d1:14fe**: one mass-storage interface
(08/06/50, bulk OUT 0x01 / IN 0x81) holding the Windows installer. It stays
there until the host sends usb_modeswitch's *HuaweiNewMode* message, a
Bulk-Only CBW carrying a vendor SCSI command:

```
55534243 12345678 00000000 00 00 0a 11062000000000000100000000000000
```

The stick drops off the bus within about 250 ms (so there is never a CSW to
read) and re-enumerates half a second later as **12d1:1506**. The driver
does this by itself on NEW_DEV; `lte switch` (or `usb switch`) repeats it by
hand. `AT^RESET` reboots the modem back into 14fe mode, which exercises the
whole path without a replug: on the test board it took 18.6 s from the
command to an IP address (3 s to drop off, 7 s to re-enumerate as 14fe,
0.6 s to come back as 1506, 8 s to register, dial and get a DHCP lease).

## Interfaces in modem mode (12d1:1506)

One configuration, four vendor-class interfaces, high speed:

| itf | class | use | endpoints |
|-----|-------|-----|-----------|
| 0 | ff/02/01 | modem (PPP) | INT IN 0x83, bulk IN 0x82, bulk OUT 0x02 |
| 1 | ff/02/03 | diagnostics | bulk IN 0x84, bulk OUT 0x03 |
| 2 | ff/02/12 | **PCUI AT port** (used) | bulk IN 0x85, bulk OUT 0x04 |
| 3 | ff/02/16 | **CDC-NCM** (used) | alt 0: INT IN 0x87; alt 1: + bulk IN 0x86, bulk OUT 0x05 |

Interface 3 carries the CDC functional descriptors on alt 0 (header 1.10,
NCM 1.00 with bmNetworkCapabilities 0x1f, Ethernet Networking with the MAC
in string descriptor 10, Union 3/3). Control and data are the same
interface, told apart by the alternate setting.

Attach sequence:

1. Claim the AT interface, send CLEAR_FEATURE(ENDPOINT_HALT) to both of its
   bulk endpoints (the host's pipes start at DATA0 while the stick carries on
   from its last toggle; without this, IN packets are silently dropped), and
   keep one 1 KB IN transfer posted at all times.
2. NCM: SET_INTERFACE alt 0, GET_NTB_PARAMETERS, SET_NTB_FORMAT(NTB16) and
   SET_CRC_MODE(0) where the stick supports them, SET_NTB_INPUT_SIZE(16384)
   (the MS2372h offers 128 KB; 16 KB is plenty and keeps the buffers small),
   SET_ETHERNET_PACKET_FILTER(directed | broadcast), read the MAC string,
   claim interface 3 alt 1 and send SET_INTERFACE alt 1 (IDF's claim does not
   send it), CLEAR_FEATURE on the bulk pair.
3. Post two 16 KB bulk IN transfers and the notification transfer, create
   the esp_netif `LTE` and start it.

The MS2372h reports: formats NTB16+NTB32, in max 131072, out max 16384,
divisor 4, remainder 2, alignment 4. All USB buffers come from
`usb_host_transfer_alloc`, which with `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM`
puts them in PSRAM.

Data framing (NCM 1.0, NTB16): every transmitted Ethernet frame is one NTB
(NTH16, then the NDP16 at offset 12, then the frame at the first offset that
satisfies the stick's divisor and remainder). A block that would be a whole
number of 512-byte packets gets one pad byte so the transfer ends with a
short packet. Received NTBs are walked through every chained NDP16; each
datagram is copied into its own PSRAM buffer for lwIP
(`CONFIG_LWIP_L2_TO_L3_COPY` is off, so lwIP holds the buffer and frees it
through the driver). Malformed NTBs are counted in `lte` status.

## AT flow

The `lte` task drives the stick through one AT command at a time,
recognising unsolicited lines (`^RSSI`, `^HCSQ`, `^NDISSTAT`, `^STIN`, ...)
wherever they turn up:

1. `AT` until OK (1 s apart, 10 tries), `ATE0`, `ATI` (model, revision,
   IMEI), `AT+CPIN?` (anything but READY stops in *SIM error*),
   `AT+CGDCONT?` (the APN of profile 1).
2. Registration: `AT+CEREG?` and `AT^SYSINFOEX` every 2 s until registered
   (stat 1 or 5, or service status 2); *no service* after 90 s, still
   polling. Then `AT+COPS?` for the operator.
3. If a session is already up (`AT^NDISSTATQRY?` says 1, e.g. after a reboot
   of the board but not of the stick) it is adopted. Otherwise, with
   auto-dial on: `AT^NDISDUP=1,1,"<apn>"`, where the APN is the stored one
   or, if none is stored, profile 1's. Up when `^NDISSTAT:1` arrives (or
   `AT^NDISSTATQRY?` says so, polled every 2 s for 30 s).
4. The netif is marked connected and its DHCP client runs (the stick answers
   DHCP itself). If no lease arrives in 15 s the driver reads `AT^DHCP?`
   (little-endian hex addresses) and configures the address, gateway and DNS
   statically; `lte` status says so.
5. `^NDISSTAT:0` takes the netif down; with auto-dial the driver redials
   10 s later. `AT^NDISSTATQRY?` every 30 s catches a drop whose URC was
   lost; `AT+CSQ` every 30 s when no signal URC has arrived. T-Mobile was
   seen to end a session by itself (`^NDISSTAT:0,36`) once in testing; the
   redial brought it back in under 12 s.

`AT^SETPORT` is not supported on this firmware, so there is no ECM mode to
switch to; NCM is the only data path.

## Routing and DNS

Each interface has a route priority: Wi-Fi station 100, LTE 50. `lsnet`
re-chooses the default interface on every GOT_IP/LOST_IP and Wi-Fi
disconnect: the preferred interface if it has an address, otherwise the
highest-priority one that has. It pins the choice with
`esp_netif_set_default_netif` and puts that interface's DNS servers back
into lwIP, whose resolver list is global. With
`CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF` (set by `lte.defaults`) each
interface keeps the servers its own DHCP gave it; without it `lsnet` notes
them at each GOT_IP. Either way the carrier's resolvers (10.177.0.x on
T-Mobile, reachable only over LTE) are not used while Wi-Fi carries the
route.

The preference is stored in NVS namespace `lsnet`: `auto` (default), or an
interface key (`WIFI_STA_DEF`, `LTE`).

## Console

`lte`
: state, model/revision/IMEI, operator and technology, RSSI/RSRP/SINR/RSRQ,
  APN, MAC and NTB sizes, address/gateway/DNS, session time, rx/tx frame and
  byte counters, drops and malformed NTBs.

`lte at <cmd>`
: one AT command through the driver's channel (5 s). The console strips
  double quotes, so write `'` for one: `lte at AT^NDISDUP=1,1,'h2g2'`.

`lte dial` / `lte hangup`
: start a session now / end it and hold auto-dial until the next `lte dial`.

`lte apn <apn>` / `lte apn -`
: store an APN (used from the next dial) / go back to profile 1's.

`lte auto on|off`
: stored auto-dial switch.

`lte switch`
: send the mode switch to a stick that is still in mass-storage mode.

`net`
: every interface with key, state, priority, address, gateway and DNS; `*`
  marks the default route.

`net prefer auto|wifi|lte`
: stored routing preference.

`net ping <host> [n]`
: ICMP echo bound to the default interface.

`net http <url>`
: one HTTP GET over the default route: status, size, time and the first 200
  bytes of a text body. `net http http://ifconfig.me/ip` shows which way the
  traffic left (the home address via Wi-Fi, a T-Mobile address via LTE).

`usb`, `usb switch [hex]`, `usb at [-i N] <cmd>`
: low-level diagnostics: descriptors of whatever is on the port, a raw mode
  switch, and one AT command on any interface (fails while the LTE driver
  holds the AT port; use `lte at`).

## Memory

The `lte` task has a 5 KB internal-RAM stack (about 2.4 KB used). The driver
state, AT buffers, NTB buffers (2 x 16 KB in, 4 x 2 KB out) and every received
frame live in PSRAM. Measured on the T-Display-P4 with the stick connected
and a session up, free internal heap at the console is about 8 KB lower than
the same board without `CONFIG_LS_LTE` (66.5 KB against 74.7 KB). The lte
task never touches NVS or SPIFFS; settings are read on the boot task and
written from the console.

## Limits

- No ECM or PPP: NCM only, and only for sticks with Huawei's vendor-class
  NCM interface.
- IPv4 only (the session is dialled `"IPV4"`; the netif has no IPv6).
- One datagram per transmitted NTB. Fine at LTE rates; received NTBs carry
  several datagrams and are all delivered.
- One stick at a time.
- Throughput was about 150 kB/s for a 1 MB HTTP download, limited by the
  single TCP connection and lwIP's 32 KB window rather than by the stick.
