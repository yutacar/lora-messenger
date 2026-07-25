# Phase 8 — BLE and Wi-Fi acceptance

Phase 8 adds local, serverless transports while preserving the protocol's bounded
behavior and its strict distinction between local broadcast acceptance and peer
delivery. Wi-Fi LAN broadcast is the first software path. BLE advertising remains
behind a physical capability gate.

## Scope and interoperability

- Wi-Fi uses LoRa Messenger protocol v1 over IPv4 UDP directed broadcast. It is
  intentionally not compatible with IP Messenger UDP/2425.
- The default project port is UDP 42425. It is unprivileged and may be changed in
  a future persisted setting after device acceptance.
- No Internet server, router traversal, relay, attachment, presence list, delivery
  ACK, encryption, authentication, or history synchronization is added.
- LAN and BLE broadcasts are public to compatible listeners in range. Wi-Fi link
  encryption does not prevent another permitted LAN client from receiving or
  spoofing application datagrams.

Primary references:

- CardputerZero Wi-Fi and BT 4.2 BLE:
  <https://docs.m5stack.com/en/CardputerZero>
- Bluetooth legacy advertising payload and no-ACK behavior:
  <https://www.bluetooth.com/bluetooth-le-primer/>
- BlueZ LE advertising:
  <https://bluez.readthedocs.io/en/latest/advertising-api/>
- BlueZ LE discovery:
  <https://bluez.readthedocs.io/en/latest/adapter-api/>
- IP Messenger ports, for the explicitly excluded compatibility mode:
  <https://ipmsg.org/help/ipmsghlp.htm>

## Current Wi-Fi preview

On CardputerZero, opt in before launching:

```sh
LORA_MESSENGER_WIFI_BROADCAST=1 \
/usr/share/APPLaunch/bin/lora-messenger-launch
```

The preview defaults to `wlan0`. If the measured OS uses another name:

```sh
LORA_MESSENGER_WIFI_BROADCAST=1 \
LORA_MESSENGER_WIFI_INTERFACE=wlp1s0 \
/usr/share/APPLaunch/bin/lora-messenger-launch
```

During Phase 8B, Wi-Fi takes precedence when explicitly enabled. If it is not
enabled, the existing antenna-confirmed LoRa startup path is unchanged. Concurrent
fan-out is Phase 8D because the current durable delivery model has only one
aggregate terminal state.

The POSIX socket:

- binds UDP 42425 on `0.0.0.0` with nonblocking I/O;
- recomputes the selected interface's directed broadcast address before each send;
- accepts only UDP 42425 sources inside that interface's current IPv4 subnet;
- ignores packets sourced by the local interface;
- consumes but rejects truncated, wrong-port, off-subnet, and malformed protocol
  datagrams;
- closes on fatal socket errors and reports link loss without claiming delivery.

## Phase 8A physical capability record

Record exact output as the normal application user:

```sh
uname -a
ip -brief link
ip -4 -brief address
iw dev
rfkill list
bluetoothctl show
btmgmt info
busctl introspect org.bluez /org/bluez/hci0
```

Then verify:

1. The selected Wi-Fi interface appears and has an IPv4 address/netmask/broadcast.
2. Two devices on the same non-isolated AP can exchange UDP 42425 broadcasts.
3. Disconnect, reconnect, and DHCP change remain bounded and do not claim delivery.
4. `hci0` exposes broadcaster and observer/central roles to the application user.
5. A non-connectable custom service-data advertisement can be registered.
6. LE scanning reports every service-data change with duplicate filtering disabled.
7. Advertising and scanning work simultaneously on one controller.
8. Measure net application bytes per legacy advertisement. Do not count the
   31-byte controller payload as entirely available to the application.
9. Rotate at least 1,000 numbered payloads; record update rate, missing/duplicate/
   reordered numbers, CPU, current draw, and error responses.
10. Repeat with Wi-Fi traffic active to expose controller coexistence limitations.

Physical result on 2026-07-24: pending. The development Mac exposed serial devices,
but none identified itself as CardputerZero through the inspected USB inventory.
No CardputerZero Wi-Fi/BlueZ interface or RF transmission was accessed.

## Wi-Fi two-device acceptance

Use two physical devices A and B on the same private AP:

1. Start without the opt-in variable. Confirm no UDP socket is used and existing
   LoRa/local behavior is unchanged.
2. Opt in on both. Confirm `Wi-Fi LAN` appears and neither device says Delivered.
3. Send one byte and a 160-byte UTF-8 body in both directions. Confirm exact post,
   reply, mention, ordering, and one durable row per message.
4. Replay duplicate UDP frames before and after receiver restart. Confirm one row
   and persistent suppression.
5. Inject wrong-port, zero-length, oversized, truncated, malformed, CRC-damaged,
   off-subnet, and local-reflection packets. Confirm bounded rejection.
6. Remove the IPv4 address, disconnect the AP, reconnect, and renew DHCP. Record
   queued/failed behavior and confirm no false Broadcast.
7. Enable AP client isolation. Confirm failure is bounded and peer delivery is not
   implied.
8. Generate sustained local traffic. Confirm the byte bucket/minimum gap prevents
   an unbounded send loop and the UI remains responsive.
9. Exit with queued/repeated work and restart. Confirm clean socket closure and no
   silent retransmission of interrupted posts.
10. Record AP model/configuration, OS image, kernel, interface name, addresses,
    latency/loss, current draw, and results.

## BLE proof-of-concept gate

The current protocol frame has a 28-byte header and minimum transport MTU 48.
CardputerZero documents BT 4.2; a legacy advertisement has at most 31 total
advertisement-data bytes before service-data overhead. BLE therefore requires a
second bounded chunk/reassembly layer. Do not implement or advertise BLE transport
support until the physical update-rate and simultaneous scan/advertise checks above
pass.

If the gate fails, stop. A GATT connection or an external BLE 5 controller changes
the requested connectionless-broadcast behavior and requires separate approval.
