# Hawk HK510 / HK520 / HK530 (USB and 2.4 GHz receiver)

Hawk Hub drives the HK500-series keyboards through a read-only device-info request. The driver
only sends GET_DEVICE_INFO and never changes settings, lighting or modes.

## Identity and command channels

| Link | USB ids | Collection | Report |
|---|---|---|---|
| USB cable | `0C45:8006` (HK510), `0C45:8009` (HK520 / HK530) | usage page `FF68`, usage `61` | 64-byte, one 56-byte frame |
| 2.4 GHz receiver | `0C45:FEFE`, product string `2.4G Dongle`, interface 3 | usage page `FF60`, usage `61` | 33-byte (report id 0 + 32 bytes), three frames |

HK520 and HK530 share the USB product id; the USB product string contains `HK520` or `HK530`.
On the receiver the string is always `2.4G Dongle`, so both are shown as `Hawk HK520/HK530`.

The reply's identity block names the paired keyboard: `0C45:8006` is the HK510, `0C45:8009` the
HK520 / HK530. The same receiver id is also used by AULA F87 Pro V2 and AJAZZ keyboards, so the
driver leaves the device unclaimed when the reply does not name a Hawk model.

HK540 (`36B0:304F`, usage page `FF00`) uses a different chip and an incompatible protocol; Hawk Hub
itself skips it ("protokol uyumsuz"), so this driver does not claim it.

## GET_DEVICE_INFO (`0x10`)

```
request   AA 10 <chunkLen> <addrLo> <addrHi> 00 <last> 00 ...
reply     55 10 <chunkLen> <addrLo> <addrHi> .. .. ..    data starts at byte 8
```

The 56-byte block is split by the report's payload size (report length minus report id minus the
8-byte header):

| Link | Request | Chunk | Address | Last |
|---|---|---|---|---|
| 2.4 GHz | 1 | 24 | 0  | 0 |
| 2.4 GHz | 2 | 24 | 24 | 0 |
| 2.4 GHz | 3 | 8  | 48 | 1 |
| USB | 1 | 56 | 0 | 1 |

Replies echo the command and address; their payload is copied to the matching offset of the
56-byte block. A collection that numbers its reports keeps the report id in front of the reply;
the driver uses the first input/output report id of the collection and strips it before parsing.

## Device-info block

| Offset | Meaning |
|---|---|
| 4..5 | paired keyboard VID (little-endian) |
| 6..7 | paired keyboard PID (little-endian) |
| 8..9 | firmware version |
| 16 | work mode: 0 USB, 1 Bluetooth, 2 2.4 GHz |
| 17 | battery level, 0..100 |
| 18 | charge state: 0 not charging, 1 charging, 2 full |

The level is reported only for a live 2.4 GHz reading. In USB mode Hawk Hub treats the level as
not meaningful and shows only the charge state; this driver does the same (percentage -1, with
charging or full set). The USB keyboard is preferred when the cable and the receiver are both
attached, and the receiver is then claimed so the keyboard is not listed twice. When no reply
arrives at all, the entry is kept for up to 30 minutes and shown as asleep (with the last value),
so the keyboard does not disappear from the tray over a single missed poll.

## Source and verification

The frame layout, command, field offsets, USB ids, command usage pages and the USB-mode semantics
come from the HK500 adapter in the installed Hawk Hub Electron bundle (`app.asar`, renderer
bundle: `GET_DEVICE_INFO`, `HK500_DEVICE_IDS`, `commandUsagePageFor`, `getBatteryStatus`). The query
was confirmed on an HK510 receiver: it answered with `0C45:8006`, work mode 2, 87 % and charge
state 0. The USB path is implemented from the same code but has not been exercised on hardware yet.

The receiver sometimes ignores a whole poll even while the keyboard is in use: 5 of 30 first
requests went unanswered. Sending the same request again 100 ms later answered 30 of 30 times, so
each command is retried once (500 ms per attempt). Before that retry, a single missed poll made
the app show the keyboard as asleep until the next successful poll.
