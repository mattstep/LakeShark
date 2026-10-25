# T-Display-P4: wired Ethernet

The T-Display-P4 has an RJ45 jack and an IC Plus IP101GRI PHY on the
ESP32-P4's internal EMAC, over RMII. LakeShark can bring it up as a DHCP
network interface next to Wi-Fi. It is off in the stock build. Build it in
with the `boards/eth.defaults` overlay:

```
idf.py -B build_tdp4_eth -D SDKCONFIG=build_tdp4_eth/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults;boards/eth.defaults" build
```

The code is in `components/lseth/`. The pins are in
`components/lakeshark/board/variants/t_display_p4.h`.

**Status:** this compiles, but nobody has run it on a board yet. Everything
below comes from the vendor sources and the datasheet, not from a
measurement.

## The trade-off: the BOOT button

**GPIO35 is both the BOOT button and RMII TXD1.** When Ethernet runs, the
EMAC drives that pin, so the BOOT button cannot cycle modes. In a build with
`CONFIG_LS_ETH`:

| `eth` setting | GPIO35 | BOOT button | Wired network |
|---|---|---|---|
| on (default) | RMII TXD1 | off: not configured, not polled | yes |
| off | GPIO input with pull-up | cycles P25 -> ADS-B -> FM | no, PHY held in reset |

The setting takes effect at the next boot. GPIO35 gets configured early in
boot, before anything could change it safely. If Ethernet is on but fails to
start (for example, the PHY does not answer), the driver releases every EMAC
pin and boot gives the button back for that run. The console's `mode next`
command still cycles modes either way.

A build without `CONFIG_LS_ETH` does exactly what it did before: it never
touches the PHY reset, and the button always works.

## Hardware

| Signal | ESP32-P4 | Notes |
|---|---|---|
| MDC | GPIO31 | |
| MDIO | GPIO52 | |
| REF_CLK (50 MHz) | GPIO50 | input to the P4 (`EMAC_CLK_EXT_IN`). The P4 never drives it |
| TX_EN | GPIO49 | |
| TXD0 | GPIO34 | |
| TXD1 | GPIO35 | also the BOOT button |
| CRS_DV | GPIO28 | |
| RXD0 | GPIO29 | |
| RXD1 | GPIO30 | |
| PHY reset (RESET_N) | XL9535 IO5 | not a GPIO, so `reset_gpio_num = -1` |
| PHY MDIO address | 1 | |

Sources:

- LilyGO `lilygo_device_driver`, `src/device/t_display_p4/t_display_p4_config.h`
  (the submodule commit pinned by `Xinyuan-LilyGO/T-Display-P4` main,
  a47cf73e). This file has `gpio::ip101::kRmiiMdc = 31`, `kRmiiMdio = 52`,
  `kRmiiRefClk = 50`, `kRmiiClkOut = -1`, `kRmiiTxEn = 49`, `kRmiiTxd0 = 34`,
  `kRmiiTxd1 = 35`, `kRmiiCrsDv = 28`, `kRmiiRxd0 = 29`, `kRmiiRxd1 = 30`,
  `kPhyRst = -1`, `device::ip101::kPhyAddress = 1`, and
  `gpio::xl9535::kEthernetRst = kIo5`.
- The same repository's `t_display_p4_driver.cpp`. `SetEthernetPowerEnabled()`
  writes `kEthernetRst` (high to run the PHY), and `InitXl9535()` drives it
  low as an output at init.
- The vendor's `main/examples/ethernet_iperf/main.cpp` and `sdkconfig.defaults`
  (`CONFIG_ETHERNET_PHY_IP101=y`, `CONFIG_ETHERNET_PHY_RST_GPIO=-1`). Neither
  sets pins, so they use the defaults of Espressif's `ethernet_init` component.
- ESP-IDF 5.5.4's `ETH_ESP32_EMAC_DEFAULT_CONFIG()` for the P4 uses the same
  pins and clock mode. The vendor and IDF agree. The board file still spells
  out every pin, so a change in IDF's defaults cannot move them.

### Reset timing

IP101GRI datasheet (IP101G-DS-R01, May 2014, `docs/IP101GRI.pdf` in the
vendor repository), Table 15:

- Trst, reset held low: at least 10 ms.
- Tclk_MII_rdy, RMII clock output valid after release: at most 10 ms.
- The chip can operate 2.5 ms after RESET_N rises (pin description).

`ls_eth_start()` drives IO5 low for 15 ms, releases it, and waits 20 ms
before it creates the EMAC. The EMAC's software reset does not complete
without the 50 MHz reference clock.

## Routing

The Ethernet netif is `if_key "ETH"`, `if_desc "eth"`, DHCP client, with
`route_prio 120`. esp_netif chooses the default route by `route_prio` among
the interfaces that are up:

- wired: 120
- Wi-Fi station: 100 (`esp_netif_create_default_wifi_sta`)
- LTE: below Wi-Fi (its own branch)

So when a cable is plugged in and DHCP answers, wired becomes the default
route. Wi-Fi takes over again when the link drops. Both can be up at once.

lwIP keeps one global DNS server list, and each DHCP lease writes to it. The
last interface to get a lease sets the DNS servers. A later shared
route/DNS module will choose between them. This module only exposes
`ls_eth_netif()`, `ls_eth_link_up()`, `ls_eth_has_ip()` and
`ls_eth_status()`.

## Console

```
eth            status: setting, link, speed/duplex, MAC, IP/mask/gw/DNS,
               link changes, time since link up, route,
               the receive task's spare stack
eth on         use Ethernet from the next boot (BOOT button off)
eth off        BOOT button from the next boot (no wired network)
```

`eth on|off` writes NVS namespace `lseth`, key `en` (u8, default 1), and
asks for a reboot.

## Memory

The EMAC allocates its DMA buffers and descriptors from internal RAM
(`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`). About 62 KB of internal RAM is
free after boot, and IDF's P4 defaults would take 17 KB of it.
`boards/eth.defaults` sets:

| | IDF P4 default | eth.defaults |
|---|---|---|
| RX buffers x 512 B | 20 | 8 |
| TX buffers x 512 B | 10 | 4 |
| descriptors (64 B each on the P4) | 30 | 12 |
| DMA total | 17,280 B | 6,912 B |
| `emac_rx` task stack | 4,096 B | 3,072 B |

Static internal RAM grows by 504 bytes: 400 B of IRAM for the EMAC ISR
and 104 B of `.bss` (from `esp_idf_size`, stock build against the
eth.defaults build). That comes to about 11 KB of internal RAM in all,
counting heap headers and the task's TCB. IDF's defaults would take about
22 KB. Received frames
are copied out of the ring into ordinary `malloc` buffers, which go to
PSRAM on this build (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`). The driver
object, the netif and the lwIP netif are small.

The smaller rings cost burst throughput. Eight RX buffers hold 2.7
full-size frames. Four TX buffers hold one full-size frame and a small one,
and lwIP retries anything the driver refuses. LakeShark's traffic (map
tiles, OTA, the file page) does not need line rate.

Ethernet starts right after Wi-Fi in boot, before USB enumeration
fragments internal RAM, for the same reason Wi-Fi starts there.

## Not yet verified on hardware

- That the PHY answers at MDIO address 1. If it does not, boot logs
  `no IP101 answering at MDIO address 1`, tears everything down and gives
  the button back.
- Where the 50 MHz reference clock comes from: a separate oscillator, or
  the IP101 itself in its 50 MHz output mode. Either one works with
  `EMAC_CLK_EXT_IN` on GPIO50, as long as the clock is running by the time
  the reset settle time ends.
- Link, DHCP, the default-route switchover, and throughput with the
  smaller rings.
- The `emac_rx` stack margin at 3 KB. `eth` prints the high-water mark.
- Whether holding the PHY in reset with `eth off` saves measurable current.
