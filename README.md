# Emu68 USBNET Direct

**A hardware-validated SANA-II Ethernet driver for PiStorm Classic that drives the Raspberry Pi 3A+ DWC2 USB controller directly from 68k code.**

This experiment turns a PiStorm-equipped Amiga into a native USB CDC-NCM Ethernet device without placing the USB networking datapath behind an ARM-side worker.

From AmigaOS, networking is exposed as a normal **SANA-II device**:

```text
usbnet.device
```

From the connected PC, the same machine appears as a standard **USB CDC-NCM Ethernet adapter**:

```text
PiStorm USB Ethernet
```

The result is a direct path:

```text
AmigaOS / MiamiDX
        |
        v
   SANA-II API
        |
        v
  usbnet.device
   (68k code)
        |
        v
 BCM2837 DWC2
 direct MMIO + DMA
        |
        v
 USB CDC-NCM
        |
        v
 Windows / Linux / macOS host
```

This README describes the **Direct-DWC2 68k** implementation and the hardware-validated `POC2N1` milestone.

---

## Current milestone

```text
Emu68 USBNET Direct-DWC2 68k POC2N1
RX IRQ Fast Path
UnitTask priority +1
Hardware validated
17 September 2026
```

This is the fastest and most stable Direct-DWC2 version validated so far.

Observed sustained RX throughput during testing was approximately:

```text
1,180,000 - 1,200,000 bytes/s
```

while preserving reliable bidirectional Ethernet operation.

The milestone intentionally keeps the proven DWC2 ownership and packet-granularity model rather than increasing complexity merely to chase larger transfer batches.

---

## What makes the Direct version different

The central idea is simple:

> **`usbnet.device` itself owns the BCM2837 DWC2 controller.**

There is no separate ARM networking worker between SANA-II and USB.

The 68k driver accesses the Pi peripherals exposed by Emu68 and operates DWC2 directly through MMIO.

The USB controller is mapped at:

```text
0xF2980000
```

with the required byte-swapped register accesses handled inside the driver.

This gives the architecture a very short datapath:

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

The experiment is therefore as much a demonstration of what the PiStorm/Emu68 platform can expose to AmigaOS as it is a network driver.

---

## USB side

The device implements **USB CDC-NCM** using the BCM2837 DWC2 controller in peripheral/device mode.

Host-visible identification:

| Property | Value |
| --- | --- |
| Manufacturer | `PiStorm` |
| Product | `PiStorm USB Ethernet` |
| USB VID | `0x0525` |
| USB PID | `0xA4AC` |
| USB class | CDC-NCM |
| NCM format | NTB16 |
| Bulk OUT endpoint | EP1 |
| Bulk IN endpoint | EP2 |
| Notification endpoint | EP3 |
| Bulk maximum packet size | 512 bytes |
| Maximum NTB | 2048 bytes |
| Maximum Ethernet datagram | 1514 bytes |

The implementation also exposes the Microsoft OS descriptor identifying the interface as:

```text
WINNCM
```

allowing compatible Windows versions to bind their native NCM driver without a custom host driver.

---

## Amiga side

The Amiga interface is a conventional **SANA-II network device**.

The current station address is:

```text
02:68:00:00:00:01
```

Supported commands include:

```text
CMD_READ
CMD_WRITE

S2_DEVICEQUERY
S2_GETSTATIONADDRESS
S2_CONFIGINTERFACE

S2_ADDMULTICASTADDRESS
S2_DELMULTICASTADDRESS
S2_MULTICAST
S2_BROADCAST

S2_TRACKTYPE
S2_UNTRACKTYPE

S2_GETTYPESTATS
S2_GETSPECIALSTATS
S2_GETGLOBALSTATS

S2_READORPHAN
S2_ONLINE
S2_OFFLINE

NSCMD_DEVICEQUERY
```

The driver has been developed and tested with **MiamiDX**, but the interface is deliberately SANA-II rather than Miami-specific.

---

## Receive architecture

RX performance was the most sensitive part of the Direct-DWC2 design.

The validated solution splits only the truly urgent USB work from the heavier networking work.

### DWC2 IRQ fast path

When EP1 OUT completes, the DWC2 interrupt reaches the Amiga as an external interrupt.

At interrupt level, the driver performs only the work required to keep USB moving:

```text
EP1 OUT completion
       |
       v
dma_end / CachePostDMA
       |
       v
copy completed 0..512-byte packet
into IRQ handoff ring
       |
       v
immediately re-arm the same DMA buffer
       |
       v
Signal(UnitTask)
```

The IRQ path deliberately does **not** perform:

- NCM parsing;
- Ethernet frame delivery;
- `CopyToBuff`;
- `ReplyMsg`;
- SANA-II request completion.

Those remain in task context.

This keeps the interrupt handler short while removing the expensive delay between an EP1 completion and the next OUT transaction.

---

## Single DMA owner

The validated RX path deliberately retains a very conservative DWC2 transaction model:

```text
EP1 OUT transfer size = 512 bytes
PKTCNT                = 1
DMA buffers active    = 1
```

A second static buffer exists in the source layout, but this milestone uses one DMA owner.

When a packet completes:

1. the DMA transaction is completed;
2. its payload is copied into the software handoff ring;
3. the same DMA buffer is immediately re-armed;
4. higher-level processing happens later.

This preserves the ownership rules of the known-good datapath while greatly improving responsiveness.

---

## IRQ handoff ring

Between the IRQ handler and the task-level NCM parser is a small single-producer/single-consumer ring:

```text
32 slots
512 bytes per slot
```

Ownership is simple:

```text
IRQ       = sole producer
UnitTask  = sole consumer
```

Each slot corresponds to one packet-granular DWC2 completion.

The ring exists only to decouple **USB hardware urgency** from **SANA-II processing latency**.

It is not an additional network queue intended to accumulate large bursts.

---

## UnitTask

All non-trivial network processing remains in a dedicated Exec task:

```text
usbnet.unit
```

The task owns:

- NCM assembly and parsing;
- receive-frame queues;
- pending SANA-II reads;
- `CopyToBuff`;
- `CopyFromBuff`;
- request completion;
- `ReplyMsg`;
- TX preparation;
- bounded DWC2 service outside the RX IRQ fast path.

`BeginIO()` therefore remains lightweight: requests are posted to the unit task rather than executing the whole network operation in the caller's context.

### Priority +1

For this milestone:

```text
usbnet.unit priority = +1
```

This small priority increase proved useful for RX.

The intent is not to monopolize the 68k scheduler. It simply prevents a normal-priority network application from pre-empting `usbnet.unit` at every frame completion before the bounded RX drain reaches its next `Wait()`.

The result was measurably better sustained receive behaviour without moving SANA-II work into interrupt context.

---

## CDC-NCM datapath

Ethernet frames are transported in standard **CDC-NCM NTB16** containers.

Receive:

```text
USB EP1 OUT
   |
512-byte DWC2 DMA completions
   |
IRQ handoff ring
   |
NCM NTB assembly
   |
NDP/datagram parsing
   |
Ethernet frame
   |
SANA-II CMD_READ / S2_READORPHAN
```

Transmit:

```text
SANA-II CMD_WRITE
   |
Ethernet frame
   |
NCM NTB16 generation
   |
USB EP2 IN
   |
DWC2 buffer DMA
```

EP3 is used for CDC network notifications, including host carrier state.

---

## DWC2 interrupt path

The Pi 3 legacy interrupt controller exposes USB as GPU interrupt 9 in IRQ bank 1.

Emu68 delivers the corresponding ARM interrupt to the Amiga side as an external interrupt.

The driver verifies both levels before claiming it:

```text
BCM IRQ pending bit
        +
masked DWC2 interrupt status
```

For RX, only the urgent EP1 OUT transfer-complete event is consumed directly by the fast path.

Other USB causes remain for the regular task-level DWC2 service.

This preserves a useful separation:

```text
hardware deadline work  -> interrupt
protocol / OS work      -> task
```

---

## VBlank fallback

The DWC2 IRQ is the primary event source in this milestone.

A lightweight VBlank path remains as a watchdog/fallback mechanism.

It is not the normal high-performance receive datapath.

The design therefore does not depend on continuously polling USB from VBlank to sustain traffic.

---

## Performance

The current hardware-validated milestone reached approximately:

```text
1.18 - 1.20 MB/s RX
```

in sustained testing.

This is especially significant because the driver retains:

- 512-byte EP1 OUT transfers;
- `PKTCNT=1`;
- one DMA owner;
- SANA-II delivery in task context;
- normal Exec request/reply semantics.

The gain came primarily from reducing the dead time between DWC2 OUT completions rather than from enlarging USB transactions.

This was an important development result: **latency in re-arming EP1 mattered more than simply making the buffering deeper.**

---

## A useful negative result

A later experiment attempted to batch multiple RX `ReplyMsg()` operations.

That variant could cause receive throughput to collapse completely.

It is intentionally **not** included in this milestone.

The validated POC2N1 behaviour is:

```text
IRQ:
    urgent DWC2 work only

UnitTask:
    parse
    deliver
    CopyToBuff
    ReplyMsg normally
```

The conservative completion model is therefore part of the milestone, not an accidental omission.

---

## Building `usbnet.device`

The Direct driver is intentionally self-contained.

Using Bebbo's Amiga GCC toolchain:

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

The resulting binary is a native AmigaOS device.

Install it in:

```text
DEVS:Networks/usbnet.device
```

or the location expected by the chosen SANA-II network stack configuration.

---

## MiamiDX

A typical configuration presents `usbnet.device` to MiamiDX as an Ethernet-style SANA-II interface.

For the development network, a commonly used setup was:

```text
PC / host : 192.168.137.1
Amiga     : 192.168.137.2
```

but these addresses are not hard-coded into the device and may be replaced by any suitable IP configuration.

`usbnet.device` operates below IP and does not depend on a particular subnet.

Once the interface has been configured and brought online, MiamiDX can use it like another SANA-II Ethernet adapter.

---

## What has been validated

The Direct-DWC2 development path has demonstrated:

- successful DWC2 initialization from 68k code;
- USB device enumeration;
- native host CDC-NCM binding;
- EP0 control request handling;
- CDC network-connection notification;
- NCM NTB16 RX and TX;
- bidirectional Ethernet;
- SANA-II integration;
- MiamiDX operation;
- DWC2 buffer DMA;
- true DWC2 interrupt delivery to AmigaOS;
- direct EP1 OUT IRQ servicing;
- immediate RX DMA re-arm;
- interrupt-to-task SPSC handoff;
- UnitTask-owned SANA-II completion;
- stable sustained RX in the ~1.2 MB/s range.

The milestone is hardware validated on the PiStorm Classic / Raspberry Pi 3A+ development platform.

---

## Design principles

Several choices in this driver are intentionally conservative.

### Keep one owner for hardware state

The DWC2 DMA transaction is never ambiguously owned by multiple execution contexts.

### Interrupt only what must be urgent

The IRQ handler exists to prevent USB bus idle time, not to run a network stack at IPL6.

### Keep SANA-II semantics in task context

`CopyToBuff`, `CopyFromBuff`, queue ownership and `ReplyMsg` remain ordinary Exec/task operations.

### Prefer measured behaviour over theoretical batching

Larger queues and more batching are not automatically faster.

Several experiments showed that reducing scheduling latency can matter more than increasing batch size.

### Preserve a known-good milestone

The source is intentionally labelled as a validation milestone rather than a finished production driver.

Changes that improve one benchmark but damage long-running stability are not automatically carried forward.

---

## Source

The milestone is intentionally compact:

```text
usbnet.device.c
MILESTONE.txt
```

`usbnet.device.c` contains:

- Amiga resident/device glue;
- SANA-II implementation;
- DWC2 register definitions;
- Pi mailbox USB power control;
- USB descriptors;
- CDC-NCM control requests;
- NCM RX/TX;
- buffer-DMA handling;
- IRQ handling;
- `usbnet.unit`;
- RX/TX rings;
- statistics and lifecycle handling.

This makes the Direct experiment unusually easy to inspect: the entire network path can be followed through one source file.

---

## Status

This is still an **experimental proof of concept**, not a production-quality network driver.

The milestone is valuable because it provides a known-good reference point for:

- direct DWC2 access from AmigaOS;
- SANA-II over USB CDC-NCM;
- PiStorm peripheral exposure;
- IRQ-driven USB networking;
- low-overhead 68k networking experiments.

Areas that still deserve further work include:

- extended compatibility testing;
- more formal error recovery;
- USB reset/disconnect corner cases;
- complete production-quality statistics;
- additional long-duration stress testing;
- cleanup and modularization of the monolithic source;
- further scheduler/latency optimization without destabilizing the validated datapath.

---

## Credits

This is an **unofficial experimental Emu68 / PiStorm extension**.

It is not an official Emu68 release.

The work depends on the Emu68 and PiStorm ecosystems and on the standard AmigaOS SANA-II networking model.

The direct DWC2 implementation also builds on knowledge gained from earlier Raspberry Pi peripheral experiments, including the DWC2/MMIO and mailbox conventions validated on PiStorm Classic.

Upstream Emu68:

```text
https://github.com/michalsc/Emu68
```

---

## License

The Direct `usbnet.device` source is distributed under:

```text
SPDX-License-Identifier: MPL-2.0
```

See the source and the licensing requirements of the surrounding projects before redistributing complete firmware or derived builds.
