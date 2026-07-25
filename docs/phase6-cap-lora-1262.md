# Phase 6 — Cap LoRa-1262 setup and physical acceptance

This document covers the Zero-compatible M5Stack Cap LoRa-1262 used by LoRa
Messenger. The software, ARM64 build, and package gates are complete. The checks
under “Two-device acceptance” require two physical CardputerZero units and remain
open.

## Hardware and safety

- Use the CardputerZero-compatible Cap LoRa-1262 (M5Stack SKU U214, SX1262,
  868–923 MHz).
- Connect the supplied/approved RP-SMA antenna before powering the Cap. Never
  transmit without an antenna.
- Confirm the unit and antenna supplied for the Japanese market and retain their
  labels and documentation. The application profile is constrained for Japan, but
  software is not a substitute for checking the exact hardware in hand.
- Power down before attaching, removing, or reseating the Cap.

Official references:

- Cap LoRa-1262: <https://docs.m5stack.com/en/cap/Cap_LoRa-1262>
- CardputerZero pins: <https://docs.m5stack.com/en/CardputerZero>
- CardputerZero launch/accessory description:
  <https://shop.m5stack.com/blogs/news/m5stack-launches-cardputerzero-a-pocket-sized-linux-computer-for-makers-and-developers>

## Zero connector mapping

| Cap signal | CardputerZero signal | Linux use |
| --- | --- | --- |
| RST | GPIO26 / HAT_P0 | GPIO output |
| IRQ | GPIO23 / HAT_P1 | GPIO input |
| BUSY | GPIO22 | GPIO input |
| SCK | GPIO11 / SPI0_CLK | `/dev/spidev0.1` |
| MOSI | GPIO10 / SPI0_MOSI | `/dev/spidev0.1` |
| MISO | GPIO9 / SPI0_MISO | `/dev/spidev0.1` |
| NSS | GPIO7 / SPI0_CS1 | `/dev/spidev0.1` |
| I²C SDA/SCL | GPIO2 / GPIO3 | `/dev/i2c-1` |

The Cap's PI4IOE5V6408 I/O expander is addressed at `0x43`; P0 enables the antenna
switch. The driver enables it only after all required device nodes open, configures
the SX1262 with a 3.0 V TCXO setting, and disables it again during shutdown.

## Fixed radio profile

| Setting | Value |
| --- | --- |
| Center frequency | 920.8 MHz |
| Bandwidth | 125 kHz |
| Spreading factor | SF9 |
| Coding rate | 4/7 |
| Transmit power | 13 dBm |
| Preamble | 12 symbols |
| Sync word | `0x12` (private) |
| Listen-before-talk | busy at or above -90 dBm |
| App airtime budget | 6,000 ms per 60,000 ms |
| Minimum TX gap | 100 ms |

The policy validates the whole occupied channel against 920.5–923.0 MHz, which is
the intersection used here between the selected Japanese range and the Cap's
documented upper limit. A profile outside these limits is rejected at startup.

## Startup

Install the `.deb`, attach the antenna, and first verify that the application user
can open the three default nodes:

```sh
test -r /dev/spidev0.1
test -r /dev/gpiochip0
test -r /dev/i2c-1
```

Then launch with the explicit physical antenna acknowledgement:

```sh
LORA_MESSENGER_ANTENNA_ATTACHED=1 \
/usr/share/APPLaunch/bin/lora-messenger-launch
```

If the OS image assigns different paths, set one or more of:

```sh
LORA_MESSENGER_SPI_DEVICE=/dev/spidev0.1
LORA_MESSENGER_GPIO_CHIP=/dev/gpiochip0
LORA_MESSENGER_I2C_DEVICE=/dev/i2c-1
```

The header changes from `LOCAL` to `JP LORA` only after initialization succeeds.
If the acknowledgement is absent or any open/configuration step fails, the radio
stays disabled. Do not run the app as root to work around permissions; configure
the CardputerZero OS device groups/rules and retest as the normal app user.

## Two-device acceptance

Perform these checks with two units, A and B, using the same fixed profile:

1. Cold-start both units with antennas attached. Confirm `JP LORA` appears and no
   transmission occurs before a post is submitted.
2. Send the minimum one-byte body from A. Confirm A reaches `Broadcast`, B records
   exactly one `Received` post, and both UIs still state that peer delivery is not
   confirmed.
3. Send a 160-byte body and a post that requires multiple frames. Confirm exact
   UTF-8 content, reply, mention, sender, and ordering on B.
4. Replay captured duplicate frames without changing their bytes. Confirm B keeps
   one timeline entry and the duplicate remains suppressed after B restarts.
5. Submit until the airtime/congestion budget delays work. Confirm the UI remains
   responsive, memory is bounded, and queued posts are neither skipped nor claimed
   delivered.
6. Create channel activity from B while A attempts to send. Confirm A's
   listen-before-talk path defers rather than collides immediately.
7. Disconnect or fault each SPI, IRQ/BUSY GPIO, and I²C path in a safe powered-down
   setup, then start again. Confirm bounded failure, disabled radio state, and no
   false `Broadcast`.
8. Exit with queued and active work. Confirm safe cancellation, SX1262 sleep, Cap
   antenna-switch disable, clean restart, and no silent retransmission of
   interrupted posts.
9. Exercise Home/exit, window/process termination, local-data recovery, and
   confirmed local-data deletion with the radio enabled.
10. Record OS image, kernel, device-node permissions, two hardware/antenna labels,
    measured range/RSSI, current draw, results, and any deviation before marking
    Phase 6 complete.

`Broadcast` means only that the local radio accepted all primary fragments. The
protocol intentionally has no peer acknowledgement, encryption, authentication,
private messages, or history synchronization.
