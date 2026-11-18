# stm32-esp32-wifi

Use an ESP32 as a Wi-Fi co-processor for an STM32 host, using
[ESP-Hosted](https://github.com/espressif/esp-hosted-mcu) over SPI (full duplex).

The STM32 side runs no RTOS. It is a HAL superloop with a small host stack of its own that speaks
the ESP-Hosted wire protocol, plus [lwIP](https://savannah.nongnu.org/projects/lwip/) in `NO_SYS` mode.
The ESP-Hosted host stack from Espressif needs an RTOS and ESP-IDF APIs, and v3.1.0 has no STM32 port,
so it is not used on the STM32.

## Status

Tested with an ESP32-C3 (YD-ESP32-C3) and an STM32F407VET6:

- SPI link with the coprocessor reset by the host, and the ESP-Hosted init handshake (RPC v2)
- RPC: firmware version, MAC, Wi-Fi mode, start, scan, connect, Wi-Fi events
- lwIP: DHCP with a hostname (`stm32-xxxxxx`, derived from the STM32 unique ID), ARP, ICMP (ping)
- Ping round trip around 10 ms. The host turns the ESP's Wi-Fi power save off (`WIFI_PS_NONE`),
  which costs extra current on the ESP. With the default modem sleep it's 30 to 230 ms.

Known issue: the YD-ESP32-C3 only runs Wi-Fi with a USB cable on one of its ports. Powered from 3.3 V
on the backplane alone, it boots and completes the host handshake, but its brownout supervisor resets
it as soon as the radio starts (`rst:0xf (BROWNOUT_RST)` at `phy_init`), even with the brownout
detector disabled in software and the TX power capped at 10 dBm.

The ESP32-S3 coprocessor firmware builds and boots but hasn't been tested with the STM32 host yet.

## Layout

```text
coprocessor/                     ESP-IDF project for the ESP32 side (S3 and C3)
esp_hosted/                      submodule: ESP-Hosted v3.1.0 (ESP side only)
lwip/                            submodule: lwIP STABLE-2_2_1_RELEASE (STM32 side)
examples/sl_mcu_stm32f407_first  STM32CubeIDE project, STM32F407 host
```

| Submodule    | Used by       | Version                | Upstream                                        |
| :----------- | :------------ | :--------------------- | :---------------------------------------------- |
| `esp_hosted` | ESP32 (IDF)   | `v3.1.0`               | https://github.com/espressif/esp-hosted-mcu     |
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

## Co-processor firmware (ESP32 side)

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

## STM32 host

`examples/sl_mcu_stm32f407_first` is an STM32CubeIDE project for the STM32F407VET6.

### Building

Open the `examples` folder as the workspace in STM32CubeIDE (tested with 2.2.0) and build
`sl_mcu_stm32f407_first`. lwIP is linked into the project from the `lwip` submodule
(`../../lwip/src`), so the submodules must be checked out.

The Wi-Fi credentials are not in git. Create `Core/Inc/wifi_secrets.h`:

```c
#ifndef WIFI_SECRETS_H_
#define WIFI_SECRETS_H_

#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"

#endif /* WIFI_SECRETS_H_ */
```

The project can also be built without the IDE, using a scratch workspace (doesn't touch an open IDE):

```bash
~/st/stm32cubeide_2.2.0/stm32cubeide --launcher.suppressErrors -nosplash -application org.eclipse.cdt.managedbuilder.core.headlessbuild -data /tmp/cubeide_ws -import examples/sl_mcu_stm32f407_first -build sl_mcu_stm32f407_first/Debug
```

Flash with openocd (or from the IDE):

```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program examples/sl_mcu_stm32f407_first/Debug/sl_mcu_stm32f407_first.elf verify reset exit"
```

The log goes to USART1 (PA9/PA10) at 921600 baud.

### Code

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

### Wiring

SPI1 on the STM32, SPI full duplex mode 3 at 10.5 MHz. The host resets the coprocessor through its EN pin.

| Signal     | STM32F407        | ESP32-C3 | ESP32-S3 | Direction    |
| :--------- | :--------------: | :------: | :------: | :----------- |
| SCK        | PA5              | GPIO6    | GPIO12   | host → ESP   |
| MOSI       | PA7              | GPIO7    | GPIO11   | host → ESP   |
| MISO       | PA6              | GPIO2    | GPIO13   | ESP → host   |
| CS         | PA4              | GPIO10   | GPIO10   | host → ESP   |
| Handshake  | PC4              | GPIO3    | GPIO17   | ESP → host   |
| Data ready | PC5              | GPIO4    | GPIO4    | ESP → host   |
| Reset      | PA3 (open drain) | EN (RST) | EN (RST) | host → ESP   |
| GND        | GND              | GND      | GND      |              |

Keep the jumpers short. The reset line matters: if the STM32 restarts while the ESP keeps running,
the ESP's SPI slave can end up out of step and every frame from the host arrives corrupted.
