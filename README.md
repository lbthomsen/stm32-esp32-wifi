# stm32-esp32-wifi

Use an ESP32 as a Wi-Fi co-processor for an STM32 host, over SPI. There are two examples, one for
each way of splitting the work:

| Example                      | ESP firmware                                                  | TCP/IP runs on | STM32 flash |
| :--------------------------- | :------------------------------------------------------------ | :------------- | :---------- |
| `sl_mcu_stm32f407_hosted`    | [ESP-Hosted](https://github.com/espressif/esp-hosted-mcu) (SPI full duplex) | STM32 (lwIP)   | ~66 KB      |
| `sl_mcu_stm32f407_at`        | [ESP-AT](https://github.com/espressif/esp-at) (SPI half duplex) | ESP          | ~28 KB      |

With ESP-Hosted the ESP is a Wi-Fi network interface: Ethernet frames go over SPI and
[lwIP](https://savannah.nongnu.org/projects/lwip/) runs on the STM32. With ESP-AT the ESP keeps the
TCP/IP stack, and the STM32 drives it with AT commands (like an offloaded TCP/IP chip).

The STM32 side runs no RTOS in either example. It is a HAL superloop with small drivers of its own.
The ESP-Hosted host stack from Espressif needs an RTOS and ESP-IDF APIs, and v3.1.0 has no STM32 port,
so it is not used on the STM32.

## Status

ESP-Hosted (`sl_mcu_stm32f407_hosted`), tested with an ESP32-C3 (YD-ESP32-C3) and an STM32F407VET6:

- SPI link with the coprocessor reset by the host, and the ESP-Hosted init handshake (RPC v2)
- RPC: firmware version, MAC, Wi-Fi mode, start, scan, connect, Wi-Fi events
- lwIP: DHCP with a hostname (`stm32-xxxxxx`, derived from the STM32 unique ID), ARP, ICMP (ping)
- Ping round trip around 10 ms. The host turns the ESP's Wi-Fi power save off (`WIFI_PS_NONE`),
  which costs extra current on the ESP. With the default modem sleep it's 30 to 230 ms.
- Link supervision: the host notices a coprocessor restart or a silent coprocessor (probe after 10 s
  without traffic), resets it through EN and reconnects.

ESP-AT (`sl_mcu_stm32f407_at`), same boards, ESP-AT v5.0.1.0 on the C3:

- SPI link (half duplex, handshake), AT commands, modem sleep off, DHCP hostname, join
- Interrupt-driven SPI: the handshake interrupt and SPI DMA completions run every transfer, the
  main loop only parses AT lines and runs the services
- Ping round trip around 3 to 9 ms (answered by the ESP)
- Web server on port 80: `GET /` returns a status page (time, NIST sync, uptime, Wi-Fi RSSI, IP, SPI
  counters), refreshing every 10 s
- UTC time from NIST's daytime service (`time.nist.gov`, TCP port 13) at start-up and every 10
  minutes, kept between syncs on the STM32's tick
- Link supervision: an ESP restart (`ready`) or a silent ESP (`AT` probe after 10 s) resets it
  through EN and rejoins

Known issue: the YD-ESP32-C3 only runs Wi-Fi with a USB cable on one of its ports. Powered from 3.3 V
on the backplane alone, it boots and completes the host handshake, but its brownout supervisor resets
it as soon as the radio starts (`rst:0xf (BROWNOUT_RST)` at `phy_init`), even with the brownout
detector disabled in software and the TX power capped at 10 dBm.

The ESP32-S3 coprocessor firmware builds and boots but hasn't been tested with the STM32 host yet.

## Layout

```text
coprocessor/                      ESP-IDF project: ESP-Hosted coprocessor firmware (S3 and C3)
at_firmware/                      build script and settings for the ESP-AT firmware (C3, SPI)
esp_hosted/                       submodule: ESP-Hosted v3.1.0 (ESP side only)
esp_at/                           submodule: ESP-AT v5.0.1.0
lwip/                             submodule: lwIP STABLE-2_2_1_RELEASE (STM32 side, _hosted only)
examples/sl_mcu_stm32f407_hosted  STM32CubeIDE project, STM32F407 host (ESP-Hosted, lwIP on the STM32)
examples/sl_mcu_stm32f407_at      STM32CubeIDE project, STM32F407 host (ESP-AT over SPI, TCP/IP on the ESP)
```

| Submodule    | Used by       | Version                | Upstream                                        |
| :----------- | :------------ | :--------------------- | :---------------------------------------------- |
| `esp_hosted` | ESP32 (IDF)   | `v3.1.0`               | https://github.com/espressif/esp-hosted-mcu     |
| `esp_at`     | ESP32         | `v5.0.1.0`             | https://github.com/espressif/esp-at             |
| `lwip`       | STM32         | `STABLE-2_2_1_RELEASE` | https://github.com/lwip-tcpip/lwip              |

`esp_hosted` has to be checked out in a directory called `esp_hosted`, because ESP-IDF names a
component after its directory.

## Getting the source

```bash
git clone --recurse-submodules git@github.com:lbthomsen/stm32-esp32-wifi.git
```

For a clone that already exists:

```bash
git submodule update --init --recursive
```

## ESP-Hosted coprocessor firmware (ESP32 side, for `sl_mcu_stm32f407_hosted`)

Needs ESP-IDF v5.5 or later. Built and tested with ESP-IDF `release/v6.2`. After checking out an
ESP-IDF branch, update its submodules too, or the build fails in mbedtls:

```bash
cd ~/esp/esp-idf && git checkout release/v6.2 && git submodule update --init --recursive && ./install.sh
```

```bash
. ~/esp/esp-idf/export.sh
```

```bash
cd coprocessor
```

The firmware is a thin ESP-IDF app around the ESP-Hosted coprocessor component. Its `app_main`
initialises Wi-Fi with IDF defaults, so the host doesn't have to send a full `wifi_init_config_t`.
Each target gets its own build directory and sdkconfig. `sdkconfig.defaults` holds the common
settings and `sdkconfig.defaults.<target>` holds the pins.

### ESP32-C3

```bash
idf.py -B build_esp32c3 -D SDKCONFIG=sdkconfig.esp32c3 set-target esp32c3
```

```bash
idf.py -B build_esp32c3 -D SDKCONFIG=sdkconfig.esp32c3 -p /dev/ttyACM1 build flash monitor
```

A board that previously ran other firmware (e.g. ESP-AT) should be erased first:

```bash
idf.py -B build_esp32c3 -D SDKCONFIG=sdkconfig.esp32c3 -p /dev/ttyACM1 erase-flash
```

### ESP32-S3

```bash
idf.py -B build_esp32s3 -D SDKCONFIG=sdkconfig.esp32s3 set-target esp32s3
```

```bash
idf.py -B build_esp32s3 -D SDKCONFIG=sdkconfig.esp32s3 -p /dev/ttyACM1 build flash monitor
```

On the S3 (chip rev v0.1, USB on the JTAG port) the automatic reset after flashing can leave the
chip in download mode (`boot:0x0 ... waiting for download`). Press RST and use `monitor --no-reset`.

### Notes

- `set-target` only needs to run once. Run it again (or delete `sdkconfig.<target>`) after changing
  `sdkconfig.defaults*`, because existing sdkconfig values take precedence over defaults.
- The ESP-Hosted settings are in `menuconfig` under *Component config → ESP-Hosted*.
- Opening the ESP's USB serial port normally resets the chip. Use `idf.py monitor --no-reset` to watch
  it without disturbing the link to the STM32.

## ESP-AT firmware (ESP32 side, for `sl_mcu_stm32f407_at`)

ESP-AT has its own build system and builds against the ESP-IDF version each release pins (`v5.0.1.0`
pins ESP-IDF `release/v5.4`). Its `build.py` clones that ESP-IDF into `esp_at/esp-idf`, applies
ESP-AT's patches to it and installs its tools. That checkout is separate from `~/esp/esp-idf`,
which isn't touched. The first build takes a while (about 1.5 GB of ESP-IDF).

`at_firmware/build.sh` runs it without the interactive questions (platform ESP32C3, module
ESP32C3-SPI, no silence mode) and adds the settings in `at_firmware/module_cfg/` on top of ESP-AT's
module defaults. Run it from a shell without ESP-IDF exported (or it unsets it anyway). It skips
ESP-AT's own `sudo apt-get install`, so the ESP-IDF prerequisites must already be there:

```bash
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv python3-setuptools cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

```bash
at_firmware/build.sh
```

```bash
at_firmware/build.sh flash /dev/ttyACM0
```

ESP-AT's SPI pins for the C3 (SCLK 6, MOSI 7, MISO 2, CS 10, handshake 3) are the same as in the
wiring table below, so the jumpers don't move between the two examples. ESP-AT doesn't use the data
ready line.

## ESP boot log

The ESP logs to two places at 115200 baud: UART0 (C3: TX GPIO21, RX GPIO20; S3: TX GPIO43, RX
GPIO44) and its built-in USB serial port (the USB-JTAG connector). Watch the USB port with:

```bash
idf.py -p /dev/ttyACM0 monitor --no-reset
```

or any terminal program that doesn't toggle DTR/RTS when opening the port. The port re-enumerates every
time the chip resets, including when the STM32 resets it through EN, so the first lines after a
reset can be missing on USB. UART0 always has the whole log.

### ESP-Hosted (`coprocessor`) on the C3

A good start, the STM32 resets the C3, then sends its init config and the RPCs (trimmed):

```text
I (441) ehcp_core:                 ESP-Hosted Firmware version :: EH-3.1.0
I (448) ehcp_core:                 Transport used :: SPI only
I (465) SPI_DRIVER: transport[cp]: SPI ctrl=1 mode=3 MOSI=7 MISO=2 CLK=6 CS=10 HANDSHAKE=3 DATA_READY=4
I (474) SPI_DRIVER: SPI Ctrl:1 mode: 3, Freq:ConfigAtHost
I (644) ehcp_core: Send slave up event
I (647) SPI_DRIVER: advertising transfer size 1600
I (663) coprocessor: ESP-Hosted coprocessor starting
I (761) ehcp_wifi_init: esp_wifi_init: 0x0
I (998) ehcp_core: Slave init_config received from host
I (1021) ehcp_core: RPC version negotiated: V2
I (1047) ehcp_event_handler: ESP init event
I (1105) phy_init: phy_version 1235,8bd8c6fc,Apr 13 2026,18:46:20
I (1138) wifi:mode : sta (dc:54:75:63:ad:30)
I (1150) wifi:Set ps type: 0, coexist: 0
I (3573) ehcp_event_handler: Wi-Fi sta scan done
I (3612) ehcp_wifi_rpc: STA set config: SSID:stm32
I (3668) wifi:connected with stm32, aid = 1, channel 9, 40D, bssid = 66:66:b3:7b:a1:4f
I (3698) ehcp_event_handler: Sta mode connected
```

The same start on the STM32 (USART1, 921600):

```text
Starting Wi-Fi (ESP-Hosted)
wifi_netif: hostname stm32-476418
esp_host: resetting coprocessor
esp_host: coprocessor up - chip_id=5 fw=3.1.0 caps=0xa0 ext_caps=0x00024010 rpc=v2 xfer=1600 q=10/10
esp_host: init reply queued
wifi_sta: coprocessor RPC ready
wifi_sta: coprocessor firmware 3.1.0 on esp32c3
wifi_sta: set mode STA -> 0
wifi_sta: STA MAC dc:54:75:63:ad:30
wifi_sta: wifi start -> 0
wifi_sta: event STA_START
wifi_sta: power save off -> 0
wifi_sta: scan start -> 0
wifi_sta: scan done
wifi_sta: connecting to "stm32"
wifi_sta: set config -> 0
wifi_sta: connect -> 0
wifi_sta: CONNECTED to "stm32" on channel 9
wifi_netif: interface up, starting DHCP
wifi_netif: IP 10.1.128.120 mask 255.255.0.0 gw 10.1.0.1
```

### ESP-AT on the C3

ESP-AT logs very little. A complete boot, ROM and bootloader included:

```text
ESP-ROM:esp32c3-api1-20210207
Build:Feb  7 2021
rst:0x15 (USB_UART_CHIP_RESET),boot:0xc (SPI_FAST_FLASH_BOOT)
SPIWP:0xee
mode:DIO, clock div:2
load:0x3fcd5820,len:0x1574
load:0x403cc710,len:0xc18
load:0x403ce710,len:0x2ffc
entry 0x403cc71a
I (24) boot: ESP-IDF v5.4.1-643-g8ad0d3d8f2f-dirty 2nd stage bootloader
I (27) boot: chip revision: v0.4
I (38) boot.esp32c3: SPI Flash Size : 4MB
I (46) boot: Partition Table:
I (49) boot: ## Label            Usage          Type ST Offset   Length
I (55) boot:  0 otadata          OTA data         01 00 0000d000 00002000
I (61) boot:  1 phy_init         RF data          01 01 0000f000 00001000
I (68) boot:  2 nvs              WiFi data        01 02 00010000 0000e000
I (74) boot:  3 at_customize     unknown          40 00 0001e000 00042000
I (81) boot:  4 ota_0            OTA app          00 10 00060000 001d0000
I (87) boot:  5 ota_1            OTA app          00 11 00230000 001d0000
I (94) boot: End of partition table
I (508) boot: Loaded app from partition at offset 0x60000
I (1025) at-init: at param mode: 1
I (1096) at-init: module_name: ESP32C3-SPI
I (1099) at-init: max tx power=78, ret=0
I (1102) at-init: v4.2.0.0-dev (unknown)
E (1103) at-spi: invalid data:0 or len:0
I (2209) at-wifi: negotiated phy mode: 5
```

Two lines look wrong but aren't: `v4.2.0.0-dev` is the version string ESP-AT v5.0.1.0 reports, and
`E (...) at-spi: invalid data:0 or len:0` shows up on every boot, good ones included. The
`ready` that the STM32 waits for goes over SPI, not to the log.

The STM32 side:

```text
Starting Wi-Fi (ESP-AT)
wifi_at: hostname stm32-476418
wifi_at: < ready
wifi_at: < WIFI CONNECTED
wifi_at: modem sleep off -> OK
wifi_at: hostname stm32-476418 -> OK
wifi_at: joining "stm32"
wifi_at: < WIFI DISCONNECT
wifi_at: < WIFI CONNECTED
wifi_at: < WIFI GOT IP
wifi_at: join "stm32" -> OK
wifi_at: ip 10.1.128.120
wifi_at: TCP server on port 80 -> OK
daytime: 2026-11-18 08:40:19 UTC from time.nist.gov
```

(ESP-AT joins with the credentials it stored last time before the STM32 asks, hence the extra
disconnect and connect.)

### What a bad link looks like

| ESP log | STM32 log | Meaning |
| :-- | :-- | :-- |
| `[frame][W] v1 checksum mismatch` or `bad offset: got=65535`, then `SPI_DRIVER: frame_decode error` | `esp_rpc: request ... timed out`, the coprocessor gets reset | Frames from the STM32 arrive corrupted (ESP-Hosted). See the slew-rate note under Wiring. |
| Boots normally, nothing about SPI | `wifi_at: no "ready" from the ESP - resetting the ESP` every 5 s | The ESP-AT status reads come back as zeros (ESP-AT). Same cause as above, or MISO not connected. |
| `rst:0xf (BROWNOUT_RST)` or `Brownout detector was triggered` after `phy_init` | the coprocessor restarts over and over | Supply can't deliver the radio's current peaks. |
| `boot:0x0 ... waiting for download` | no init event or `ready` | The ESP is in download mode (strapping pins, or the auto-reset after flashing). |

## STM32 host

`examples/sl_mcu_stm32f407_hosted` (ESP-Hosted) and `examples/sl_mcu_stm32f407_at` (ESP-AT) are
STM32CubeIDE projects for the STM32F407VET6. Both use the same pins.

### Building

Open the `examples` folder as the workspace in STM32CubeIDE (tested with 2.2.0) and build
`sl_mcu_stm32f407_hosted` or `sl_mcu_stm32f407_at`. In `_hosted`, lwIP comes into the project through
a symlink, `lwip -> ../../lwip/src`, so the submodules must be checked out.

The Wi-Fi credentials are not in git. Create `Core/Inc/wifi_secrets.h` in each project:

```c
#ifndef WIFI_SECRETS_H_
#define WIFI_SECRETS_H_

#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"

#endif /* WIFI_SECRETS_H_ */
```

The project can also be built without the IDE, using a scratch workspace (doesn't touch an open IDE):

```bash
~/st/stm32cubeide_2.2.0/stm32cubeide --launcher.suppressErrors -nosplash -application org.eclipse.cdt.managedbuilder.core.headlessbuild -data /tmp/cubeide_ws -import examples/sl_mcu_stm32f407_hosted -build sl_mcu_stm32f407_hosted/Debug
```

Flash with openocd (or from the IDE):

```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program examples/sl_mcu_stm32f407_hosted/Debug/sl_mcu_stm32f407_hosted.elf verify reset exit"
```

The log goes to USART1 (PA9/PA10) at 921600 baud.

### Code: `sl_mcu_stm32f407_hosted`

| File                | What it does                                                                 |
| :------------------ | :--------------------------------------------------------------------------- |
| `esp_spi.c`         | SPI transport: CS/handshake/data ready, framing, checksum, TX queue, reset   |
| `esp_host.c`        | Init event and host capability reply, routes frames to RPC and network       |
| `esp_rpc.c`         | RPC control path: endpoint/data TLVs, `Rpc` envelope, requests and events    |
| `pb.c`              | Minimal protobuf encoder/decoder (no malloc)                                 |
| `wifi_sta.c`        | Station state machine: mode, start, scan, connect, reconnect                 |
| `wifi_netif.c`      | lwIP netif on the STA data path, DHCP, hostname                              |
| `lwipopts.h`        | lwIP configuration (`NO_SYS`)                                                |

The RPC messages are encoded by hand from `esp_hosted/common/serializers/eh_proto_v2/src/proto/rpc_v2.proto`
instead of using the generated protobuf-c code, which is over 200 KB of flash. The whole STM32 image
is around 66 KB of flash.

### Code: `sl_mcu_stm32f407_at`

| File                | What it does                                                                 |
| :------------------ | :--------------------------------------------------------------------------- |
| `at_spi.c`          | ESP-AT SPI transport (half duplex, handshake), interrupt driven, byte streams both ways |
| `at_client.c`       | AT commands and responses, unsolicited lines, `+IPD` socket data, `CIPSEND`  |
| `wifi_at.c`         | Station (hostname, join, IP, RSSI), the TCP server, client connections, link ownership, link supervision |
| `http_server.c`     | HTTP/1.0 status page on port 80                                              |
| `daytime.c`         | NIST daytime client, UTC clock between syncs                                 |

The protocol follows ESP-AT's reference SPI master, `esp_at/examples/at_spi_master`. The whole STM32
image is around 28 KB of flash.

How the work is split:

- **Interrupts** move the bytes. A rising edge on the handshake line (EXTI4) starts a status read, and
  each SPI DMA completion (DMA2 streams 0 and 3) starts the next transaction of the sequence: read
  the data, say "read done", or write the data the ESP is ready for. Each transaction is one full
  duplex DMA transfer of the 3 header bytes plus the data. The DMA streams are set up in `at_spi.c`
  rather than in CubeMX, so regenerating the project leaves them alone. Don't enable SPI1 DMA in
  CubeMX as well.
- **The main loop** parses what arrived and runs the services. `wifi_at` hands each link's connect,
  data and close events to the service that owns it: incoming connections go to the HTTP server,
  and the daytime client gets the link it opened. The HTTP server collects the request as its data
  arrives and replies once the headers are complete. One AT command runs at a time.

ESP-AT allows one TCP server and five links in all. The server is limited to four connections
(`AT+CIPSERVERMAXCONN=4`), so a link stays free for the daytime client.

Known limit: ESP-AT accepts only one or two connections from a burst at once. With eight
simultaneous requests the rest get in as the client retries (1 s, 2 s, ...). A browser opening the
page uses at most two (the page and `favicon.ico`, which gets a 404).

NIST asks clients to query no more than once every 4 seconds; every 10 minutes is well inside that.
The time on the page is to the second (the daytime format has no fractions).

### Wiring

SPI1 on the STM32 at 10.5 MHz: mode 3 full duplex for ESP-Hosted, mode 0 half duplex for ESP-AT
(the `.ioc` of each project has its mode). The host resets the ESP through its EN pin.

| Signal     | STM32F407        | ESP32-C3 | ESP32-S3 | Direction    |
| :--------- | :--------------: | :------: | :------: | :----------- |
| SCK        | PA5              | GPIO6    | GPIO12   | host → ESP   |
| MOSI       | PA7              | GPIO7    | GPIO11   | host → ESP   |
| MISO       | PA6              | GPIO2    | GPIO13   | ESP → host   |
| CS         | PA4              | GPIO10   | GPIO10   | host → ESP   |
| Handshake  | PC4              | GPIO3    | GPIO17   | ESP → host   |
| Data ready | PC5              | GPIO4    | GPIO4    | ESP → host (ESP-Hosted only) |
| Reset      | PA3 (open drain) | EN (RST) | EN (RST) | host → ESP   |
| GND        | GND              | GND      | GND      |              |

SCK, MISO and MOSI are set to low output speed (slew rate) in both `.ioc` files. With the CubeMX
default (very high), the fast edges ring on jumper wires and the ESP's SPI slave sometimes sees an
extra clock edge. Frames from the host then arrive corrupted. This happened at 1.3 MHz as well as at
10.5 MHz, and medium speed still gave errors. Low speed gave none, and the clock edges are still
fast enough for 10.5 MHz on short wires. On a PCB with short traces the faster settings should be fine.

Keep the jumpers short. The reset line matters: if the STM32 restarts while the ESP keeps running,
the ESP's SPI slave can end up out of step and every frame from the host arrives corrupted.
