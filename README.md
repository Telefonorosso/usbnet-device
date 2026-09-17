# Emu68 USBNET Direct

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

## Recommended test network

For the simplest first test, use a private point-to-point subnet:

```text
Windows: 192.168.137.1
Amiga:   192.168.137.2
Mask:    255.255.255.0
```

In MiamiDX:

```text
IP address:      192.168.137.2
Netmask:         255.255.255.0
Gateway:         192.168.137.1
```

For pure local testing, the gateway is not required.

Bring the interface online and test:

```text
ping 192.168.137.1
```

From Windows:

```powershell
ping 192.168.137.2
```

Once this works, choose one of the two Windows configurations below.

---

# Windows setup

There are two useful ways to connect the Amiga to the rest of the network.

## Option 1 — NAT / Internet Connection Sharing

This is the easiest configuration.

Windows acts as a router between:

```text
Amiga USBNET
        |
        v
Windows
        |
        v
LAN / Wi-Fi / Internet
```

The Amiga remains on its own private subnet.

A practical configuration is:

```text
Windows USBNET: 192.168.137.1
Amiga:          192.168.137.2
Gateway:        192.168.137.1
```

### Enable Internet Connection Sharing

Open:

```text
Control Panel
→ Network and Internet
→ Network Connections
```

Identify:

1. the adapter that already has Internet/LAN access;
2. `PiStorm USB Ethernet`.

Right-click the adapter that has Internet access:

```text
Properties
→ Sharing
```

Enable:

```text
Allow other network users to connect through this computer's Internet connection
```

Select the PiStorm USB Ethernet adapter as the private/home connection.

Windows ICS uses one interface as the public side and another as the private side, performing NAT between them. citeturn357750search1turn357750search6

Depending on the current Windows configuration, ICS may automatically assign the private adapter an address. If it changes the subnet, either use the address Windows assigns or restore the Amiga configuration to match it.

For the setup used during development, the intended addressing was:

```text
Windows USBNET: 192.168.137.1
Amiga:          192.168.137.2
```

### MiamiDX settings

Use:

```text
IP:       192.168.137.2
Netmask:  255.255.255.0
Gateway:  192.168.137.1
```

DNS can be:

```text
192.168.137.1
```

or your normal LAN/router DNS server.

### Test

From Amiga:

```text
ping 192.168.137.1
```

then:

```text
ping <LAN-router-address>
```

and finally an Internet IP.

For example:

```text
ping 1.1.1.1
```

If raw IP works but hostnames do not, the remaining problem is DNS rather than USBNET.

### Advantages

- easiest setup;
- does not disturb the normal LAN;
- works even if the PC connects through Wi-Fi;
- no changes required on the home router;
- Amiga traffic is NATed behind Windows.

### Limitations

The Amiga is not a first-class member of the physical LAN.

Machines elsewhere on the LAN normally cannot initiate a connection directly to:

```text
192.168.137.2
```

unless routing or port forwarding is configured.

Use the bridge setup below if you want the Amiga to appear directly on the same Ethernet subnet as the rest of the LAN.

---

## Option 2 — Windows network bridge

With a bridge, Windows joins:

```text
PiStorm USB Ethernet
```

and the PC's physical Ethernet adapter at Layer 2.

The topology becomes:

```text
Amiga
  |
USB CDC-NCM
  |
Windows bridge
  |
physical Ethernet
  |
home LAN
```

The Amiga can then use an address from the same subnet as every other LAN machine.

Example:

```text
Router:  192.168.1.1
PC:      192.168.1.x
Amiga:   192.168.1.200
```

Microsoft documents Windows Network Bridge specifically as a way to join separate network segments so they behave as a single network. citeturn357750search0

### Important

For this setup, bridge the PiStorm adapter with a **physical Ethernet adapter** whenever possible.

Bridging to Wi-Fi is often problematic because ordinary Wi-Fi client mode does not behave exactly like transparent Ethernet bridging.

### GUI method

Open:

```text
Control Panel
→ Network and Internet
→ Network Connections
```

Select both:

```text
PiStorm USB Ethernet
```

and your physical Ethernet adapter.

Right-click and choose:

```text
Bridge Connections
```

Windows should create:

```text
Network Bridge
```

The IP configuration for the Windows PC then belongs to the bridge rather than the individual member adapters.

### Command-line diagnostics

Run an elevated Command Prompt or PowerShell:

```cmd
netsh bridge show adapter
```

Windows 11 includes `netsh bridge` commands for inspecting and managing bridge-capable adapters. citeturn357750search0

You can also list existing bridges with:

```cmd
netsh bridge list
```

### MiamiDX settings

The Amiga now belongs directly to the LAN.

For example, if your LAN is:

```text
192.168.1.0/24
```

with router:

```text
192.168.1.1
```

configure MiamiDX as:

```text
IP:       192.168.1.200
Netmask:  255.255.255.0
Gateway:  192.168.1.1
DNS:      192.168.1.1
```

Choose an unused address or use whatever addressing policy you normally use on your LAN.

### Test

From Amiga:

```text
ping 192.168.1.1
```

then ping another LAN machine.

From another machine on the LAN:

```text
ping 192.168.1.200
```

If both directions work, the bridge is operating as intended.

### Advantages

- Amiga sits directly on the home LAN;
- no NAT;
- LAN machines can initiate connections to the Amiga;
- convenient for Telnet, FTP, HTTP servers and development tools;
- behaves much more like a real Ethernet NIC.

### Limitations

- Windows bridge configuration is more sensitive than NAT;
- third-party VPNs, virtual adapters and Hyper-V switches can interfere;
- Wi-Fi adapters are not ideal bridge partners;
- Windows firewall and adapter policies can affect traffic;
- creating a bridge may temporarily disrupt the PC's own network connection.

---

# Which mode should I use?

For general use:

```text
NAT / ICS
```

is the simplest and safest choice.

Use:

```text
Windows Network Bridge
```

when you specifically want the Amiga to become a normal host on your existing LAN.

In short:

```text
NAT:
Amiga -> Windows -> LAN
```

versus:

```text
Bridge:
Amiga == LAN peer
```

---

## Windows troubleshooting

### Confirm the adapter exists

Open:

```text
ncpa.cpl
```

You should see a network adapter corresponding to:

```text
PiStorm USB Ethernet
```

If USB enumeration succeeded but the interface is not usable, check Device Manager.

The implementation exposes CDC-NCM and a Microsoft-compatible `WINNCM` descriptor so supported Windows versions can bind a native NCM driver.

---

### Check addresses

In PowerShell:

```powershell
ipconfig /all
```

For the NAT setup, verify that the USBNET adapter and Amiga are on the same private subnet.

Typical example:

```text
Windows: 192.168.137.1
Amiga:   192.168.137.2
```

---

### Test only the direct USB link first

Before debugging routing, NAT or DNS, confirm:

```text
Windows -> Amiga
Amiga   -> Windows
```

with ping.

Do not debug Internet access until this works reliably.

---

### Hyper-V / virtual adapters

If the PC contains adapters such as:

```text
vEthernet
Hyper-V Virtual Ethernet Adapter
VPN adapters
virtual switches
```

make sure you are sharing or bridging the correct physical adapter.

Virtual network components can modify routing, interface metrics and bridge eligibility.

---

### Firewall

If ping or incoming services fail, temporarily test with the relevant Windows firewall profile disabled or create an explicit rule.

Re-enable normal firewall protection after diagnosing the problem.

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
