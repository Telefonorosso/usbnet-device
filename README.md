# Emu68 USBNET Direct - AI CODED

**Direct 68k SANA-II networking for Emu68 — driving the PiStorm DWC2 USB controller natively as a CDC-NCM Ethernet adapter.**

This project turns a PiStorm-equipped Amiga into a USB Ethernet device.

On AmigaOS you get:

```text
usbnet.device
```

On Windows you get a standard USB CDC-NCM network adapter:

```text
PiStorm USB Ethernet
```

No custom Windows driver is required.

The important part of this version is that **`usbnet.device` itself drives the BCM2837 DWC2 USB controller directly from 68k code**.

---

## Quick overview

```text
AmigaOS / MiamiDX
        |
        v
   SANA-II API
        |
        v
  usbnet.device
        |
        v
 BCM2837 DWC2
 direct MMIO + DMA
        |
        v
 USB CDC-NCM
        |
        v
      Windows
```

Validated milestone:

```text
Emu68 USBNET Direct-DWC2 68k POC2N1
RX IRQ Fast Path
UnitTask priority +1
Hardware validated
17 September 2026
```

Observed sustained RX throughput:

```text
~1.18 - 1.20 MB/s
```

---

## Requirements

- PiStorm Classic
- Raspberry Pi 3A+
- Emu68 base compatible with this milestone
- AmigaOS with MiamiDX
- USB cable between the Pi USB device port and the Windows PC
- Bebbo GCC if you want to rebuild `usbnet.device`

---

## Install `usbnet.device`

Copy:

```text
usbnet.device
```

to:

```text
DEVS:Networks/
```

or another location used by your SANA-II configuration.

Then configure MiamiDX to use:

```text
usbnet.device
```

as an Ethernet-style SANA-II interface.

---

## Windows networking

The USB link itself uses:

```text
Windows: 192.168.137.1
Amiga:   192.168.137.2
Mask:    255.255.255.0
```

Configure MiamiDX with `192.168.137.2/24`.

### Option 1 — NAT

Open **PowerShell as Administrator** and create the NAT:

```powershell
New-NetNat -Name "AmigaUSB" -InternalIPInterfaceAddressPrefix "192.168.137.0/24"
```

To remove it later:

```powershell
Remove-NetNat -Name "AmigaUSB" -Confirm:$false
```

### Option 2 — Network Bridge

Open **Control Panel → Network Connections**, select both **PiStorm USB Ethernet** and the physical Ethernet adapter, then right-click and choose **Bridge Connections**.

With a bridge, configure the Amiga with an unused address from your normal LAN instead of `192.168.137.2`.

---

## Device details

Host-visible USB information:

| Property | Value |
| --- | --- |
| Manufacturer | `PiStorm` |
| Product | `PiStorm USB Ethernet` |
| USB VID | `0x0525` |
| USB PID | `0xA4AC` |
| USB class | CDC-NCM |
| NCM format | NTB16 |
| Bulk OUT | EP1 |
| Bulk IN | EP2 |
| Notification | EP3 |
| Bulk MPS | 512 bytes |
| Max NTB | 2048 bytes |
| Max Ethernet frame | 1514 bytes |

Amiga MAC address:

```text
02:68:00:00:00:01
```

---

## Driver architecture

The Direct variant has an intentionally short path:

```text
MiamiDX
   |
SANA-II
   |
usbnet.device
   |
DWC2 DMA
   |
USB
```

`usbnet.device` owns the BCM2837 DWC2 controller directly.

There is no separate networking service in the USB datapath.

---

## RX fast path

The high-performance receive path is split between interrupt and task context.

On EP1 OUT completion:

```text
DWC2 IRQ
   |
finish DMA
   |
copy <=512-byte USB packet
into handoff ring
   |
immediately re-arm EP1
   |
signal usbnet.unit
```

The interrupt handler does **not** perform:

```text
NCM parsing
CopyToBuff
ReplyMsg
SANA-II delivery
```

Those remain in task context.

This dramatically reduces the idle time between USB OUT transactions.

---

## `usbnet.unit`

The Exec task:

```text
usbnet.unit
```

handles:

- NCM assembly;
- NCM parsing;
- Ethernet frame delivery;
- pending `CMD_READ` requests;
- `CopyToBuff`;
- `CopyFromBuff`;
- TX preparation;
- `ReplyMsg`;
- normal SANA-II request completion.

Milestone task priority:

```text
+1
```

This small priority increase improved sustained RX without moving OS-level work into interrupt context.

---

## CDC-NCM transport

RX:

```text
EP1 OUT
  |
512-byte USB packets
  |
IRQ handoff ring
  |
NCM NTB16 assembly
  |
Ethernet frame
  |
SANA-II
```

TX:

```text
SANA-II CMD_WRITE
  |
Ethernet frame
  |
NCM NTB16
  |
EP2 IN
```

---

## Performance

Hardware-validated sustained receive throughput:

```text
~1.18 - 1.20 MB/s
```

This is achieved while retaining:

```text
EP1 transfer size = 512 bytes
PKTCNT            = 1
single DMA owner
SANA-II delivery  = task context
```

The major performance improvement came from **re-arming the USB OUT endpoint immediately in the IRQ path**, rather than increasing transfer size or introducing aggressive batching.

---

## Build

Using Bebbo GCC:

```bash
m68k-amigaos-gcc \
  -O3 \
  -Wall \
  -Wextra \
  -fomit-frame-pointer \
  -noixemul \
  -nostartfiles \
  -I/opt/amiga/m68k-amigaos/ndk-include \
  -I/root/sana/include \
  usbnet.device.c \
  -o usbnet.device
```

Install the resulting binary as:

```text
DEVS:Networks/usbnet.device
```

---

## Validated features

The milestone has demonstrated:

- direct BCM2837 DWC2 access from 68k code;
- USB device enumeration;
- Windows CDC-NCM binding;
- EP0 control handling;
- NCM NTB16 RX/TX;
- bidirectional Ethernet;
- SANA-II operation;
- MiamiDX operation;
- DWC2 DMA;
- real DWC2 IRQ delivery;
- RX IRQ fast path;
- immediate EP1 re-arm;
- IRQ-to-task handoff;
- sustained RX around 1.2 MB/s;
- stable Telnet and normal TCP/IP use in the validated configuration.

---

## Status

This remains an experimental proof of concept, but it is a hardware-validated and highly usable milestone.

Potential future work includes:

- longer stress testing;
- more complete statistics;
- additional USB reset/reconnect handling;
- production-level error recovery;
- further scheduler tuning;
- source modularization;
- cleanup for eventual upstream-quality review.

---

## Credits

This is an unofficial experimental Emu68 / PiStorm extension.

It is not an official Emu68 release.

The project depends on:

- Emu68;
- PiStorm;
- AmigaOS Exec;
- the SANA-II networking API;
- MiamiDX for the principal Amiga-side validation.

Upstream Emu68:

```text
https://github.com/michalsc/Emu68
```

---

## License

The Direct `usbnet.device` source uses:

```text
SPDX-License-Identifier: MPL-2.0
```

Check the source and surrounding project licenses before redistribution.
