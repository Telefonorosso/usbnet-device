/*
 * usbnet.device DIRECT-DWC2 POC2M1 FULL-DMA
 *
 * PiStorm Classic / Emu68 / Raspberry Pi 3A+
 * AmigaOS SANA-II device that owns BCM2837 DWC2 directly from 68k code.
 *
 * POC1 scope:
 *   - no ARM CPU3 worker
 *   - no ARM-service/ZIII shared-memory transport
 *   - direct 68k MMIO at $F2980000, using byte-swapped register accesses
 *   - firmware mailbox power-on copied conceptually from berrypi3ap.device
 *   - DWC2 forced into peripheral/device mode
 *   - hardware-validated USBNET CDC-NCM EP0 descriptors/control requests
 *   - EP0 serviced from VBlank polling for first hardware validation
 *   - CDC NETWORK_CONNECTION notification for host carrier
 *   - NCM NTB16 bulk OUT/IN datapath using the DWC2 buffer-DMA backend
 *   - SANA-II RX/TX wired directly to CDC-NCM
 *
 * This file is intentionally a validation milestone, not a release driver.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <exec/types.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/interrupts.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <hardware/intbits.h>
#include <utility/tagitem.h>
#include <devices/sana2.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>

#define STR(s) #s
#define XSTR(s) STR(s)

#define DEVICE_NAME      "usbnet.device"
#define DEVICE_DATE      "(16 Sep 2026)"
#define DEVICE_VERSION   0
#define DEVICE_REVISION  17
#define DEVICE_PRIORITY  0
#define DEVICE_ID_STRING "usbnet " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE " DIRECT-DWC2-68K-POC2N1-RX-IRQ-FASTPATH-BETA"

#define ETH_HLEN            14UL
#define ARMNET_ETH_MTU      1500UL
#define ETH_FRAME_MAX       (ETH_HLEN + ARMNET_ETH_MTU)
#define PENDING_BIT         (1 << 4)
#define UNIT_TASK_STACK      16384UL
#define RX_RING_SLOTS       64U
#define RX_RING_MASK        (RX_RING_SLOTS-1U)
#define TX_RING_SLOTS       16U
#define TX_RING_MASK        (TX_RING_SLOTS-1U)
#define USB_RX_FAST_SLOTS    32U
#define USB_RX_FAST_MASK     (USB_RX_FAST_SLOTS-1U)
#define DMA_TEST_SIZE       512UL
#define DMA_READ_FROM_RAM   8UL

#ifndef NSCMD_DEVICEQUERY
#define NSCMD_DEVICEQUERY 0x4000
#endif
#define NSDEVTYPE_SANA2 7

/* ------------------------------------------------------------------------- */
/* BCM2837 / Pi3 mapping exposed by Emu68, proven by berrypi3ap.device.       */
/* ------------------------------------------------------------------------- */
#define USB2_BASE               0xF2980000UL
#define MBOX_READ_ADDR          0xF200B880UL
#define MBOX_STATUS_ADDR        0xF200B898UL
#define MBOX_WRITE_ADDR         0xF200B8A0UL

/* BCM2837 legacy interrupt controller, exposed through Emu68's Pi peripheral
 * mapping.  DWC2/USB is GPU interrupt 9 in IRQ bank 1. */
#define BCM_IRQ_PENDING1_ADDR    0xF200B204UL
#define BCM_IRQ_ENABLE1_ADDR     0xF200B210UL
#define BCM_IRQ_DISABLE1_ADDR    0xF200B21CUL
#define BCM_IRQ_USB_BIT          (1UL << 9)

#define MBOX_TX_FULL            0x80000000UL
#define MBOX_RX_EMPTY           0x40000000UL
#define MBOX_PROPERTY           8UL
#define TAG_SET_POWER           0x00028001UL
#define DEV_USB_HCD             3UL

#define USB_GAHBCFG             0x008
#define USB_GUSBCFG             0x00c
#define USB_GRSTCTL             0x010
#define USB_GINTSTS             0x014
#define USB_GINTMSK             0x018
#define USB_GRXSTSP             0x020
#define USB_GRXFSIZ             0x024
#define USB_GNPTXFSIZ           0x028
#define USB_GSNPSID             0x040
#define USB_DPTXFSIZ(n)         (0x100 + ((n) * 4))
#define USB_DCFG                0x800
#define USB_DCTL                0x804
#define USB_DSTS                0x808
#define USB_DIEPMSK             0x810
#define USB_DOEPMSK             0x814
#define USB_DAINT               0x818
#define USB_DAINTMSK            0x81c
#define USB_DIEPEMPMSK          0x834
#define USB_DIEPCTL(n)          (0x900 + ((n) * 0x20))
#define USB_DIEPINT(n)          (0x908 + ((n) * 0x20))
#define USB_DIEPTSIZ(n)         (0x910 + ((n) * 0x20))
#define USB_DIEPDMA(n)          (0x914 + ((n) * 0x20))
#define USB_DTXFSTS(n)          (0x918 + ((n) * 0x20))
#define USB_DOEPCTL(n)          (0xb00 + ((n) * 0x20))
#define USB_DOEPINT(n)          (0xb08 + ((n) * 0x20))
#define USB_DOEPTSIZ(n)         (0xb10 + ((n) * 0x20))
#define USB_DOEPDMA(n)          (0xb14 + ((n) * 0x20))
#define USB_FIFO(n)             (0x1000 + ((n) * 0x1000))

#define USB_GAHBCFG_GLBL_INTR_EN        (1UL << 0)
#define USB_GAHBCFG_DMA_EN              (1UL << 5)
#define USB_GUSBCFG_FORCEDEVMODE        (1UL << 30)
#define USB_GUSBCFG_FORCEHOSTMODE       (1UL << 29)
#define USB_GUSBCFG_HNPCAP              (1UL << 9)
#define USB_GUSBCFG_SRPCAP              (1UL << 8)
#define USB_GUSBCFG_TOUTCAL_MASK        0x7UL
#define USB_GRSTCTL_AHBIDLE             (1UL << 31)
#define USB_GRSTCTL_TXFNUM_ALL          (0x10UL << 6)
#define USB_GRSTCTL_TXFFLSH             (1UL << 5)
#define USB_GRSTCTL_RXFFLSH             (1UL << 4)
#define USB_GRSTCTL_CSFTRST             (1UL << 0)
#define USB_GINTSTS_OEPINT              (1UL << 19)
#define USB_GINTSTS_IEPINT              (1UL << 18)
#define USB_GINTSTS_ENUMDONE            (1UL << 13)
#define USB_GINTSTS_USBRST              (1UL << 12)
#define USB_GINTSTS_RXFLVL              (1UL << 4)
#define USB_GINTSTS_CURMODE_HOST        (1UL << 0)
#define USB_GINTMSK_USBNET              (USB_GINTSTS_USBRST | USB_GINTSTS_ENUMDONE | USB_GINTSTS_IEPINT | USB_GINTSTS_OEPINT)
#define USB_DCFG_DEVADDR_MASK           (0x7fUL << 4)
#define USB_DCFG_DEVADDR(a)             (((ULONG)(a) & 0x7fUL) << 4)
#define USB_DCFG_DEVSPD_MASK            3UL
#define USB_DCTL_SFTDISCON              (1UL << 1)
#define USB_DSTS_ENUMSPD_MASK           (3UL << 1)
#define USB_DAINT_INEP(n)               (1UL << (n))
#define USB_DAINT_OUTEP(n)              (1UL << ((n) + 16))
#define USB_DXEPCTL_EPENA               (1UL << 31)
#define USB_DXEPCTL_CNAK                (1UL << 26)
#define USB_DXEPCTL_SNAK                (1UL << 27)
#define USB_DXEPCTL_TXFNUM(n)           (((ULONG)(n) & 0xfUL) << 22)
#define USB_DXEPCTL_STALL               (1UL << 21)
#define USB_DXEPCTL_EPTYPE_BULK         (2UL << 18)
#define USB_DXEPCTL_EPTYPE_INTR         (3UL << 18)
#define USB_DXEPCTL_USBACTEP            (1UL << 15)
#define USB_DXEPCTL_MPS(n)              ((ULONG)(n) & 0x7ffUL)
#define USB_DXEPINT_SETUP               (1UL << 3)
#define USB_DXEPINT_XFERCOMPL           (1UL << 0)
#define USB_DXEPTSIZ_PKTCNT(n)          (((ULONG)(n) & 0x3ffUL) << 19)
#define USB_DXEPTSIZ_XFERSIZE(n)        ((ULONG)(n) & 0x7ffffUL)
#define USB_DIEPTSIZ0_PKTCNT(n)         (((ULONG)(n) & 3UL) << 19)
#define USB_DIEPTSIZ0_XFERSIZE(n)       ((ULONG)(n) & 0x7fUL)
#define USB_DOEPTSIZ0_SUPCNT(n)         (((ULONG)(n) & 3UL) << 29)
#define USB_DOEPTSIZ0_PKTCNT            (1UL << 19)
#define USB_GRXSTS_PKTSTS(v)            (((v) >> 17) & 0xfUL)
#define USB_GRXSTS_BYTECNT(v)           (((v) >> 4) & 0x7ffUL)
#define USB_GRXSTS_EPNUM(v)             ((v) & 0xfUL)
#define USB_PKTSTS_OUTRX                 2UL
#define USB_PKTSTS_OUTDONE               3UL
#define USB_PKTSTS_SETUPRX               6UL

#define USB_REQ_GET_STATUS               0x00
#define USB_REQ_CLEAR_FEATURE            0x01
#define USB_REQ_SET_ADDRESS              0x05
#define USB_REQ_GET_DESCRIPTOR           0x06
#define USB_REQ_GET_CONFIGURATION        0x08
#define USB_REQ_SET_CONFIGURATION        0x09
#define USB_REQ_GET_INTERFACE            0x0a
#define USB_REQ_SET_INTERFACE            0x0b
#define USB_DT_DEVICE                    1
#define USB_DT_CONFIG                    2
#define USB_DT_STRING                    3
#define USB_DT_DEVICE_QUALIFIER          6

#define MS_OS_STRING_INDEX               0xee
#define MS_OS_VENDOR_CODE                0x20
#define MS_OS_EXT_COMPAT_ID_INDEX        0x0004

#define NCM_GET_NTB_PARAMETERS           0x80
#define NCM_GET_NTB_FORMAT               0x83
#define NCM_SET_NTB_FORMAT               0x84
#define NCM_GET_NTB_INPUT_SIZE           0x85
#define NCM_SET_NTB_INPUT_SIZE           0x86
#define NCM_GET_MAX_DATAGRAM_SIZE        0x87
#define NCM_SET_MAX_DATAGRAM_SIZE        0x88
#define NCM_SET_CRC_MODE                 0x8a
#define CDC_SET_ETHERNET_PACKET_FILTER   0x43

#define USB_NET_EP_OUT                   1U
#define USB_NET_EP_IN                    2U
#define USB_NET_EP_NOTIFY                3U
#define USB_NET_BULK_MPS                 512U
#define USB_NET_NOTIFY_MPS               16U
#define USB_NET_NTB_MAX                  2048U
#define USB_NET_MAX_DATAGRAM             1514U

#define WBVAL(x) ((UBYTE)((x) & 0xff)), ((UBYTE)(((x) >> 8) & 0xff))
#define DBVAL(x) WBVAL((x) & 0xffff), WBVAL(((x) >> 16) & 0xffff)

struct NSDeviceQueryResult {
    ULONG DevQueryFormat;
    ULONG SizeAvailable;
    UWORD DeviceType;
    UWORD DeviceSubType;
    const UWORD *SupportedCommands;
};

struct usb_setup_packet {
    UBYTE bmRequestType;
    UBYTE bRequest;
    UWORD wValue;
    UWORD wIndex;
    UWORD wLength;
};

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
static BPTR saved_seg_list;
static BOOL is_open;
static BOOL configured;
static BOOL online;
static BOOL vblank_added;
static struct List read_q;
static struct List tx_wait_q;
static struct Interrupt vblank_int;
static struct Interrupt dwc2_irq_int;
static BOOL dwc2_irq_added;
static struct SignalSemaphore tx_sem;
static UBYTE dma_test_buf[DMA_TEST_SIZE] __attribute__((aligned(8)));
static ULONG dma_test_bus_rx;
static ULONG dma_test_bus_tx;
static UBYTE ep0_setup_dma[8] __attribute__((aligned(8)));
static UBYTE ep0_tx_dma[256] __attribute__((aligned(8)));
static UBYTE ep0_out_dma[8] __attribute__((aligned(8)));
static UBYTE notify_dma[8] __attribute__((aligned(8)));
/* EP1 OUT single-buffer diagnostic: keep the validated one-HS-packet
 * (512-byte) transfer granularity.  Slot 1 remains allocated only to preserve
 * the milestone's static layout; it is never armed or consumed in this build. */
static UBYTE bulk_out_dma[2][USB_NET_BULK_MPS] __attribute__((aligned(8)));
static UBYTE bulk_in_dma[USB_NET_NTB_MAX] __attribute__((aligned(8)));
static ULONG ep0_setup_dma_len, ep0_tx_dma_len, ep0_out_dma_len, notify_dma_len;
static ULONG bulk_out_dma_len[2], bulk_in_dma_len;
static UBYTE ep0_setup_dma_active, ep0_tx_dma_active, ep0_out_dma_active, notify_dma_active;
static volatile UBYTE bulk_out_dma_active[2];
static UBYTE bulk_out_dma_index, bulk_in_dma_active;
static struct Sana2DeviceStats stats;

typedef APTR CopyFunc;
static CopyFunc copy_to_buff;
static CopyFunc copy_from_buff;
static APTR opener_cookie;

static UBYTE hw_ready;
static UBYTE usb_configured;
static UBYTE usb_data_alt;
static UBYTE ep0_state;
static UBYTE ep0_out_kind;
static UWORD ep0_out_expected;
static ULONG ncm_input_size = USB_NET_NTB_MAX;
static UWORD ncm_max_datagram = USB_NET_MAX_DATAGRAM;
static UBYTE ep0_tx_buf[256];
static UWORD ep0_tx_len;
static UWORD ep0_tx_pos;
static UBYTE ep0_out_buf[8];
static APTR prop_alloc;
static UBYTE *prop_ptr;

/* POC2C transport state: mirror the validated TinyUSB-like milestone. */
enum { BULK_IN_IDLE=0, BULK_IN_DATA, BULK_IN_ZLP };

struct rx_slot_local { UWORD len; UBYTE data[USB_NET_MAX_DATAGRAM]; };
struct tx_slot_local { UWORD len; UBYTE data[USB_NET_NTB_MAX]; };

static UBYTE bulk_out_buf[USB_NET_NTB_MAX];
static UWORD bulk_out_len;

/* EP1 OUT IRQ fast-path handoff.  The ISR is the sole producer and UnitTask
 * the sole consumer.  Each slot contains exactly one packet-granular DWC2
 * completion (0..512 bytes).  This keeps the validated PKTCNT=1 ownership
 * model while allowing EP1 to be re-armed before the task is scheduled. */
struct usb_rx_fast_slot { UWORD len; UBYTE data[USB_NET_BULK_MPS]; };
static struct usb_rx_fast_slot usb_rx_fast_ring[USB_RX_FAST_SLOTS];
static volatile UWORD usb_rx_fast_prod, usb_rx_fast_cons;
static volatile ULONG usb_rx_fast_drops;
static UBYTE bulk_tx_buf[USB_NET_NTB_MAX];
static UWORD bulk_tx_len;
static UWORD bulk_tx_pos;
static UBYTE bulk_in_state;
static UBYTE notify_busy;
static UBYTE network_notification_sent;
static UWORD ncm_tx_seq;
static UBYTE tx_frame[ETH_FRAME_MAX];
static UBYTE tx_ntb[USB_NET_NTB_MAX];

static struct rx_slot_local rx_ring[RX_RING_SLOTS];
static volatile UWORD rx_prod, rx_cons;

/* GENET-style unit task.  BeginIO only posts messages.  This task is the
 * sole owner of read_q and the only context that completes queued SANA-II
 * requests.  POC2J: VBlank no longer services DWC2 directly; it only wakes
 * this UnitTask.  The UnitTask owns bounded DWC2 service plus all SANA-II
 * queue/completion work.  This preserves the final IRQ -> Signal(UnitTask)
 * architecture without adding a polling process or a second worker. */
static struct Process *unit_process;
static struct Task *unit_task;
static struct Task *unit_parent;
static struct MsgPort *unit_port;
static volatile BOOL unit_task_ready;
static volatile BOOL unit_task_stopping;
static BYTE unit_rx_sigbit;
static BYTE unit_abort_sigbit;
static BYTE unit_usb_sigbit;
static ULONG unit_rx_sigmask;
static ULONG unit_abort_sigmask;
static ULONG unit_usb_sigmask;

static struct tx_slot_local tx_ring[TX_RING_SLOTS];
static volatile UWORD tx_prod, tx_cons;
static UBYTE tx_slot_owned;

static BOOL call_copy(CopyFunc fn,APTR to,APTR from,ULONG len);
static BOOL do_write(struct IOSana2Req *io,BOOL bcast);
static void unit_reply_aborted(struct IOSana2Req *io);
static void unit_drain_pending_writes(void);

static const UBYTE amiga_mac[6] = {0x02,0x68,0x00,0x00,0x00,0x01};
static const UWORD supported_cmds[] = {
    CMD_READ, CMD_WRITE, S2_DEVICEQUERY, S2_GETSTATIONADDRESS,
    S2_CONFIGINTERFACE, S2_ADDMULTICASTADDRESS, S2_DELMULTICASTADDRESS,
    S2_MULTICAST, S2_BROADCAST, S2_TRACKTYPE, S2_UNTRACKTYPE,
    S2_GETTYPESTATS, S2_GETSPECIALSTATS, S2_GETGLOBALSTATS,
    S2_READORPHAN, S2_ONLINE, S2_OFFLINE, NSCMD_DEVICEQUERY, 0
};

static const UBYTE device_desc[] = {
    18, USB_DT_DEVICE, 0x00,0x02, 0xef,0x02,0x01, 64,
    0x25,0x05, 0xac,0xa4, 0x10,0x00, 1,2,3,1
};
static const UBYTE qualifier_desc[] = {
    10, USB_DT_DEVICE_QUALIFIER, 0x00,0x02, 0xef,0x02,0x01, 64,1,0
};
static const UBYTE config_desc[] = {
    9, USB_DT_CONFIG, WBVAL(94), 2,1,0,0x80,50,
    8,11,0,2,0x02,0x0d,0x00,0,
    9,4,0,0,1,0x02,0x0d,0x00,0,
    5,0x24,0x00,0x20,0x01,
    5,0x24,0x06,0,1,
    13,0x24,0x0f,4, DBVAL(0), WBVAL(USB_NET_MAX_DATAGRAM), WBVAL(0),0,
    6,0x24,0x1a,0x00,0x01,0x00,
    7,5,0x83,0x03, WBVAL(USB_NET_NOTIFY_MPS),9,
    9,4,1,0,0,0x0a,0x00,0x01,0,
    9,4,1,1,2,0x0a,0x00,0x01,0,
    7,5,0x01,0x02, WBVAL(USB_NET_BULK_MPS),0,
    7,5,0x82,0x02, WBVAL(USB_NET_BULK_MPS),0
};
static const UBYTE str0[] = {4,USB_DT_STRING,0x09,0x04};
static const UBYTE str1[] = {16,USB_DT_STRING,'P',0,'i',0,'S',0,'t',0,'o',0,'r',0,'m',0};
static const UBYTE str2[] = {42,USB_DT_STRING,'P',0,'i',0,'S',0,'t',0,'o',0,'r',0,'m',0,' ',0,'U',0,'S',0,'B',0,' ',0,'E',0,'t',0,'h',0,'e',0,'r',0,'n',0,'e',0,'t',0};
static const UBYTE str3[] = {20,USB_DT_STRING,'U',0,'S',0,'B',0,'N',0,'E',0,'T',0,'0',0,'0',0,'1',0};
static const UBYTE str4[] = {26,USB_DT_STRING,'0',0,'2',0,'0',0,'0',0,'0',0,'0',0,'0',0,'0',0,'0',0,'0',0,'0',0,'0',0,'1',0};
static const UBYTE ms_os_string_desc[] = {0x12,USB_DT_STRING,'M',0,'S',0,'F',0,'T',0,'1',0,'0',0,'0',0,MS_OS_VENDOR_CODE,0};
static const UBYTE ms_os_ext_compat_id[] = {
    0x28,0,0,0, 0,1, 4,0, 1, 0,0,0,0,0,0,0,
    0,1, 'W','I','N','N','C','M',0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0
};

static inline void init_list(struct List *l)
{
    l->lh_Head=(struct Node *)&l->lh_Tail; l->lh_Tail=NULL; l->lh_TailPred=(struct Node *)&l->lh_Head;
}
static void zero_mem(APTR p, ULONG n) { UBYTE *q=(UBYTE *)p; while(n--) *q++=0; }
static void mac_copy(UBYTE *d,const UBYTE *s){int i;for(i=0;i<6;i++)d[i]=s[i];}

static inline ULONG bswap32(ULONG v)
{
    return ((v & 0x000000ffUL) << 24) | ((v & 0x0000ff00UL) << 8) |
           ((v & 0x00ff0000UL) >> 8)  | ((v & 0xff000000UL) >> 24);
}
static inline ULONG mmio_raw_read(ULONG addr) { return *(volatile ULONG *)addr; }
static inline void mmio_raw_write(ULONG addr, ULONG v) { *(volatile ULONG *)addr=v; }
static inline ULONG rd(ULONG off) { return bswap32(mmio_raw_read(USB2_BASE+off)); }
static inline void wr(ULONG off, ULONG v) { mmio_raw_write(USB2_BASE+off,bswap32(v)); }
static inline void bcm_irq_write(ULONG addr, ULONG v) { mmio_raw_write(addr,bswap32(v)); }

static int wait_mask(ULONG off, ULONG mask, ULONG wanted, ULONG loops)
{
    while(loops--) if((rd(off)&mask)==wanted) return 1;
    return 0;
}
static void fifo_write(unsigned ep,const UBYTE *buf,ULONG len)
{
    ULONG addr=USB2_BASE+USB_FIFO(ep);
    while(len){ULONG w=0,n=len>4?4:len,i;for(i=0;i<n;i++)w|=((ULONG)buf[i])<<(8*i);mmio_raw_write(addr,bswap32(w));buf+=n;len-=n;}
}
static void fifo_read(UBYTE *buf,ULONG len)
{
    ULONG addr=USB2_BASE+USB_FIFO(0);
    while(len){ULONG w=bswap32(mmio_raw_read(addr)),n=len>4?4:len,i;for(i=0;i<n;i++)*buf++=(UBYTE)(w>>(8*i));len-=n;}
}
static void drain_fifo(ULONG len){UBYTE t[16];while(len){ULONG n=len>16?16:len;fifo_read(t,n);len-=n;}}


static ULONG dma_begin(APTR buf,ULONG *mapped_len,ULONG requested,ULONG flags)
{
    ULONG len=requested?requested:1;
    ULONG phys=(ULONG)CachePreDMA(buf,&len,flags);
    if(!phys || len<(requested?requested:1)){
        if(phys) CachePostDMA(buf,&len,flags);
        *mapped_len=0;
        return 0;
    }
    *mapped_len=len;
    return (phys & 0x3fffffffUL) | 0xc0000000UL;
}
static void dma_end(APTR buf,ULONG *mapped_len,ULONG flags)
{
    ULONG len=*mapped_len;
    if(len){CachePostDMA(buf,&len,flags);*mapped_len=0;}
}

static void store_le32(UBYTE *p,ULONG v){p[0]=(UBYTE)v;p[1]=(UBYTE)(v>>8);p[2]=(UBYTE)(v>>16);p[3]=(UBYTE)(v>>24);}
static ULONG mailbox_power_usb(void)
{
    ULONG len=32, phys, msg;
    volatile ULONG st;
    ULONG guard;
    if(!prop_alloc){
        prop_alloc=AllocMem(0x200,MEMF_FAST|MEMF_CLEAR);
        if(!prop_alloc) return 0;
        prop_ptr=(UBYTE *)(((ULONG)prop_alloc+127UL)&~127UL);
    }
    store_le32(prop_ptr+0,32); store_le32(prop_ptr+4,0); store_le32(prop_ptr+8,TAG_SET_POWER);
    store_le32(prop_ptr+12,8); store_le32(prop_ptr+16,8); store_le32(prop_ptr+20,DEV_USB_HCD);
    store_le32(prop_ptr+24,3); store_le32(prop_ptr+28,0);
    phys=(ULONG)CachePreDMA(prop_ptr,&len,0);
    msg=(phys&0xfffffff0UL)|MBOX_PROPERTY;
    guard=0x00800000UL;
    do { st=bswap32(mmio_raw_read(MBOX_STATUS_ADDR)); if(!(st&MBOX_TX_FULL)) break; } while(--guard);
    if(!guard){CachePostDMA(prop_ptr,&len,0);return 0;}
    mmio_raw_write(MBOX_WRITE_ADDR,bswap32(msg));
    guard=0x00800000UL;
    while(guard--){
        ULONG r;
        st=bswap32(mmio_raw_read(MBOX_STATUS_ADDR));
        if(st&MBOX_RX_EMPTY) continue;
        r=bswap32(mmio_raw_read(MBOX_READ_ADDR));
        if((r&0xf)==MBOX_PROPERTY && (r&0xfffffff0UL)==(msg&0xfffffff0UL)){CachePostDMA(prop_ptr,&len,0);return 1;}
    }
    CachePostDMA(prop_ptr,&len,0); return 0;
}

static void flush_fifos(void)
{
    wr(USB_GRSTCTL,USB_GRSTCTL_TXFNUM_ALL|USB_GRSTCTL_TXFFLSH|USB_GRSTCTL_RXFFLSH);
    (void)wait_mask(USB_GRSTCTL,USB_GRSTCTL_TXFFLSH|USB_GRSTCTL_RXFFLSH,0,1000000UL);
}

enum {EP0_IDLE=0,EP0_IN_DATA,EP0_OUT_STATUS,EP0_OUT_DATA,EP0_IN_STATUS};
enum {EP0_OUT_NONE=0,EP0_OUT_NTB_INPUT_SIZE,EP0_OUT_MAX_DATAGRAM};

static void ep0_arm_setup(void)
{
    ULONG bus;
    if(ep0_setup_dma_active){dma_end(ep0_setup_dma,&ep0_setup_dma_len,0);ep0_setup_dma_active=0;}
    bus=dma_begin(ep0_setup_dma,&ep0_setup_dma_len,8,0);if(!bus)return;
    ep0_setup_dma_active=1;
    wr(USB_DOEPDMA(0),bus);
    wr(USB_DOEPTSIZ(0),USB_DOEPTSIZ0_SUPCNT(1)|USB_DOEPTSIZ0_PKTCNT|8);
    wr(USB_DOEPCTL(0),USB_DXEPCTL_EPENA|USB_DXEPCTL_CNAK);
}
static void ep0_arm_out(UWORD len)
{
    ULONG bus;
    if(ep0_out_dma_active){dma_end(ep0_out_dma,&ep0_out_dma_len,0);ep0_out_dma_active=0;}
    bus=dma_begin(ep0_out_dma,&ep0_out_dma_len,len,0);if(!bus)return;
    ep0_out_dma_active=1;
    wr(USB_DOEPDMA(0),bus);
    wr(USB_DOEPTSIZ(0),USB_DOEPTSIZ0_PKTCNT|(ULONG)len);
    wr(USB_DOEPCTL(0),USB_DXEPCTL_EPENA|USB_DXEPCTL_CNAK);
}
static void ep0_send_next_chunk(void)
{
    UWORD remain,chunk,i;ULONG bus;
    if(ep0_tx_pos>=ep0_tx_len)return;
    remain=ep0_tx_len-ep0_tx_pos;chunk=remain>64?64:remain;
    for(i=0;i<chunk;i++)ep0_tx_dma[i]=ep0_tx_buf[ep0_tx_pos+i];
    if(ep0_tx_dma_active){dma_end(ep0_tx_dma,&ep0_tx_dma_len,DMA_READ_FROM_RAM);ep0_tx_dma_active=0;}
    bus=dma_begin(ep0_tx_dma,&ep0_tx_dma_len,chunk,DMA_READ_FROM_RAM);if(!bus)return;
    ep0_tx_dma_active=1;
    wr(USB_DIEPDMA(0),bus);
    wr(USB_DIEPTSIZ(0),USB_DIEPTSIZ0_PKTCNT(1)|USB_DIEPTSIZ0_XFERSIZE(chunk));
    wr(USB_DIEPCTL(0),USB_DXEPCTL_EPENA|USB_DXEPCTL_CNAK);
    ep0_tx_pos+=chunk;
}
static void ep0_send(const UBYTE *b,UWORD len){UWORD i;if(len>sizeof(ep0_tx_buf))len=sizeof(ep0_tx_buf);for(i=0;i<len;i++)ep0_tx_buf[i]=b[i];ep0_tx_len=len;ep0_tx_pos=0;if(len)ep0_send_next_chunk();}
static void ep0_zlp(void)
{
    ULONG bus;
    if(ep0_tx_dma_active){dma_end(ep0_tx_dma,&ep0_tx_dma_len,DMA_READ_FROM_RAM);ep0_tx_dma_active=0;}
    bus=dma_begin(ep0_tx_dma,&ep0_tx_dma_len,1,DMA_READ_FROM_RAM);if(!bus)return;
    ep0_tx_dma_active=1;
    wr(USB_DIEPDMA(0),bus);
    wr(USB_DIEPTSIZ(0),USB_DIEPTSIZ0_PKTCNT(1));
    wr(USB_DIEPCTL(0),USB_DXEPCTL_EPENA|USB_DXEPCTL_CNAK);
}
static void ep0_stall(void){wr(USB_DIEPCTL(0),rd(USB_DIEPCTL(0))|USB_DXEPCTL_STALL);wr(USB_DOEPCTL(0),rd(USB_DOEPCTL(0))|USB_DXEPCTL_STALL);}
static void set_address_now(UBYTE a){ULONG v=rd(USB_DCFG);v&=~USB_DCFG_DEVADDR_MASK;v|=USB_DCFG_DEVADDR(a);wr(USB_DCFG,v);}
static void put_le16(UBYTE *p,UWORD v){p[0]=(UBYTE)v;p[1]=(UBYTE)(v>>8);}
static void put_le32(UBYTE *p,ULONG v){p[0]=(UBYTE)v;p[1]=(UBYTE)(v>>8);p[2]=(UBYTE)(v>>16);p[3]=(UBYTE)(v>>24);}

static UWORD get_le16(const UBYTE *p){return (UWORD)(p[0]|((UWORD)p[1]<<8));}
static ULONG get_le32(const UBYTE *p){return (ULONG)p[0]|((ULONG)p[1]<<8)|((ULONG)p[2]<<16)|((ULONG)p[3]<<24);}
static BOOL mac_is_bcast(const UBYTE *p){int i;for(i=0;i<6;i++)if(p[i]!=0xff)return FALSE;return TRUE;}
static BOOL mac_is_mcast(const UBYTE *p){return (p[0]&1)?TRUE:FALSE;}
static void finish_io(struct IOSana2Req *io){io->ios2_Req.io_Flags&=~PENDING_BIT;if(!(io->ios2_Req.io_Flags&IOF_QUICK))ReplyMsg(&io->ios2_Req.io_Message);}
static struct IOSana2Req *find_read_for(ULONG type,BOOL *orphan)
{
    struct Node *n;struct IOSana2Req *fallback=NULL;*orphan=FALSE;
    for(n=read_q.lh_Head;n->ln_Succ;n=n->ln_Succ){struct IOSana2Req *io=(struct IOSana2Req *)n;if(io->ios2_Req.io_Command==S2_READORPHAN){if(!fallback)fallback=io;}else if(io->ios2_Req.io_Command==CMD_READ){if((io->ios2_Req.io_Flags&SANA2IOF_RAW)||io->ios2_PacketType==type)return io;}}
    if(fallback)*orphan=TRUE;
    return fallback;
}
static void deliver_frame(const UBYTE *frame,ULONG flen)
{
    struct IOSana2Req *io;ULONG type,len;BOOL orphan;APTR src;
    if(flen<ETH_HLEN){stats.BadData++;return;}
    type=((ULONG)frame[12]<<8)|frame[13];
    /* UnitTask is the exclusive owner of read_q. */
    io=find_read_for(type,&orphan);
    if(io)Remove(&io->ios2_Req.io_Message.mn_Node);
    if(!io){stats.UnknownTypesReceived++;return;}
    io->ios2_Req.io_Flags&=~(SANA2IOF_BCAST|SANA2IOF_MCAST);mac_copy(io->ios2_DstAddr,frame);mac_copy(io->ios2_SrcAddr,frame+6);if(mac_is_bcast(frame))io->ios2_Req.io_Flags|=SANA2IOF_BCAST;else if(mac_is_mcast(frame))io->ios2_Req.io_Flags|=SANA2IOF_MCAST;io->ios2_PacketType=type;
    if(io->ios2_Req.io_Flags&SANA2IOF_RAW){len=flen;src=(APTR)frame;}else{len=flen-ETH_HLEN;src=(APTR)(frame+ETH_HLEN);}io->ios2_DataLength=len;io->ios2_Req.io_Error=0;io->ios2_WireError=0;
    if(!call_copy(copy_to_buff,io->ios2_Data,src,len)){io->ios2_Req.io_Error=S2ERR_NO_RESOURCES;io->ios2_WireError=S2WERR_BUFF_ERROR;}else{stats.PacketsReceived++;if(orphan)stats.UnknownTypesReceived++;}finish_io(io);
}

static BOOL rx_enqueue_frame(const UBYTE *frame,UWORD len)
{
    UWORD prod,next,i;
    if(len<ETH_HLEN||len>USB_NET_MAX_DATAGRAM)return FALSE;
    prod=rx_prod;next=(UWORD)((prod+1U)&RX_RING_MASK);
    if(next==rx_cons)return FALSE;
    rx_ring[prod].len=len;
    for(i=0;i<len;i++)rx_ring[prod].data[i]=frame[i];
    __asm__ volatile("" ::: "memory");
    rx_prod=next;
    __asm__ volatile("" ::: "memory");
    if(unit_task_ready && unit_task && unit_rx_sigmask)
        Signal(unit_task,unit_rx_sigmask);
    return TRUE;
}

static void rx_drain(void)
{
    int budget=64;
    while(budget-->0){
        UWORD cons=rx_cons,prod=rx_prod,next,len;
        if(cons==prod)break;
        __asm__ volatile("" ::: "memory");
        len=rx_ring[cons].len;
        if(online&&len>=ETH_HLEN&&len<=USB_NET_MAX_DATAGRAM)deliver_frame(rx_ring[cons].data,len);else if(len)stats.BadData++;
        next=(UWORD)((cons+1U)&RX_RING_MASK);
        __asm__ volatile("" ::: "memory");rx_cons=next;
    }
}

static BOOL bulk_out_arm_index(UBYTE idx)
{
    ULONG bus;
    (void)idx;
    idx=0;
    /* Single-buffer diagnostic: slot 0 is the only EP1 OUT DMA owner. */
    if(bulk_out_dma_active[0])return FALSE;
    bus=dma_begin(bulk_out_dma[0],&bulk_out_dma_len[0],USB_NET_BULK_MPS,0);
    if(!bus)return FALSE;
    bulk_out_dma_active[0]=1;
    bulk_out_dma_index=0;
    wr(USB_DOEPDMA(USB_NET_EP_OUT),bus);
    wr(USB_DOEPTSIZ(USB_NET_EP_OUT),USB_DXEPTSIZ_PKTCNT(1)|USB_DXEPTSIZ_XFERSIZE(USB_NET_BULK_MPS));
    wr(USB_DOEPCTL(USB_NET_EP_OUT),rd(USB_DOEPCTL(USB_NET_EP_OUT))|USB_DXEPCTL_EPENA|USB_DXEPCTL_CNAK);
    return TRUE;
}

static void bulk_out_arm(void)
{
    (void)bulk_out_arm_index(0);
}

static int process_one_ntb(const UBYTE *b,UWORD avail)
{
    UWORD block,ndp,nlen,pos;
    if(avail<12)return 0;
    if(get_le32(b)!=0x484d434eUL||get_le16(b+4)!=12)return -1;
    block=get_le16(b+8);ndp=get_le16(b+10);
    if(block<12||block>USB_NET_NTB_MAX)return -1;
    if(avail<block)return 0;
    if(!ndp||(ULONG)ndp+16UL>block||get_le32(b+ndp)!=0x304d434eUL)return -1;
    nlen=get_le16(b+ndp+4);if(nlen<16||(ULONG)ndp+nlen>block)return -1;
    for(pos=8;pos+4<=nlen;pos+=4){
        UWORD idx=get_le16(b+ndp+pos),len=get_le16(b+ndp+pos+2);
        if(!idx&&!len)break;
        if(idx<12||len<ETH_HLEN||(ULONG)idx+len>block||len>USB_NET_MAX_DATAGRAM){stats.BadData++;continue;}
        deliver_frame(b+idx,len);
    }
    return (int)block;
}

static void process_bulk_ntbs_ready(void)
{
    while(bulk_out_len>=12){
        int used=process_one_ntb(bulk_out_buf,bulk_out_len);
        UWORD i,remain;
        if(used==0)return;
        if(used<0){bulk_out_len=0;stats.BadData++;return;}
        if((UWORD)used<bulk_out_len){remain=(UWORD)(bulk_out_len-(UWORD)used);for(i=0;i<remain;i++)bulk_out_buf[i]=bulk_out_buf[(UWORD)used+i];bulk_out_len=remain;}
        else bulk_out_len=0;
    }
}

/* Drain packet-granular data staged by the level-6 EP1 fast path.  All NCM
 * assembly/parsing and every SANA-II operation remain in UnitTask context. */
static void usb_rx_fast_drain(void)
{
    int budget=64;
    while(budget-- > 0){
        UWORD cons=usb_rx_fast_cons,prod=usb_rx_fast_prod,len,i;
        if(cons==prod)break;
        __asm__ volatile("" ::: "memory");
        len=usb_rx_fast_ring[cons].len;
        if(len<=USB_NET_BULK_MPS && (ULONG)bulk_out_len+len<=sizeof(bulk_out_buf)){
            for(i=0;i<len;i++)bulk_out_buf[bulk_out_len+i]=usb_rx_fast_ring[cons].data[i];
            bulk_out_len=(UWORD)(bulk_out_len+len);
        }else{
            bulk_out_len=0;
            stats.BadData++;
        }
        usb_rx_fast_cons=(UWORD)((cons+1U)&USB_RX_FAST_MASK);
        __asm__ volatile("" ::: "memory");
        process_bulk_ntbs_ready();
    }
    if(usb_rx_fast_cons!=usb_rx_fast_prod && unit_task_ready && unit_task && unit_usb_sigmask)
        Signal(unit_task,unit_usb_sigmask);
}

static void bulk_in_fill_fifo(void) { /* POC2M1: DWC2 DMA owns IN data movement. */ }

static void bulk_in_complete_logical(void)
{
    if(tx_slot_owned){tx_cons=(UWORD)((tx_cons+1U)&TX_RING_MASK);tx_slot_owned=0;}
    bulk_tx_len=0;bulk_tx_pos=0;bulk_in_state=BULK_IN_IDLE;
}

static BOOL tx_enqueue_ntb(const UBYTE *buf,UWORD len)
{
    UWORD prod,next,i;
    if(!len||len>USB_NET_NTB_MAX)return FALSE;

    /* SPSC: UnitTask is the sole producer, VBlank/usb_poll the sole consumer.
       Publish payload+length before tx_prod; no Disable()/Enable() in UnitTask. */
    prod=tx_prod;next=(UWORD)((prod+1U)&TX_RING_MASK);
    if(next==tx_cons)return FALSE;
    tx_ring[prod].len=len;
    for(i=0;i<len;i++)tx_ring[prod].data[i]=buf[i];
    __asm__ volatile("" ::: "memory");
    tx_prod=next;
    return TRUE;
}

static void tx_kick(void)
{
    UWORD cons,len,i;ULONG packets,bus;
    if(!usb_configured||!usb_data_alt||bulk_in_state!=BULK_IN_IDLE||tx_cons==tx_prod)return;
    cons=tx_cons;len=tx_ring[cons].len;
    if(!len||len>USB_NET_NTB_MAX){tx_cons=(UWORD)((cons+1U)&TX_RING_MASK);return;}
    for(i=0;i<len;i++)bulk_in_dma[i]=tx_ring[cons].data[i];
    bulk_tx_len=len;bulk_tx_pos=len;tx_slot_owned=1;bulk_in_state=BULK_IN_DATA;
    if(bulk_in_dma_active){dma_end(bulk_in_dma,&bulk_in_dma_len,DMA_READ_FROM_RAM);bulk_in_dma_active=0;}
    bus=dma_begin(bulk_in_dma,&bulk_in_dma_len,len,DMA_READ_FROM_RAM);if(!bus){bulk_in_complete_logical();return;}
    bulk_in_dma_active=1;
    packets=((ULONG)len+USB_NET_BULK_MPS-1UL)/USB_NET_BULK_MPS;
    wr(USB_DIEPDMA(USB_NET_EP_IN),bus);
    wr(USB_DIEPTSIZ(USB_NET_EP_IN),USB_DXEPTSIZ_PKTCNT(packets)|USB_DXEPTSIZ_XFERSIZE(len));
    wr(USB_DIEPCTL(USB_NET_EP_IN),USB_DXEPCTL_MPS(USB_NET_BULK_MPS)|USB_DXEPCTL_USBACTEP|USB_DXEPCTL_EPTYPE_BULK|USB_DXEPCTL_TXFNUM(USB_NET_EP_IN)|USB_DXEPCTL_EPENA|USB_DXEPCTL_CNAK);
}

static UWORD ncm_build_ntb(const UBYTE *frame,UWORD flen)
{
    UWORD off=28,total=(UWORD)(off+flen),i;if(total>USB_NET_NTB_MAX)return 0;for(i=0;i<off;i++)tx_ntb[i]=0;put_le32(tx_ntb,0x484d434eUL);put_le16(tx_ntb+4,12);put_le16(tx_ntb+6,ncm_tx_seq++);put_le16(tx_ntb+8,total);put_le16(tx_ntb+10,12);put_le32(tx_ntb+12,0x304d434eUL);put_le16(tx_ntb+16,16);put_le16(tx_ntb+18,0);put_le16(tx_ntb+20,off);put_le16(tx_ntb+22,flen);put_le16(tx_ntb+24,0);put_le16(tx_ntb+26,0);for(i=0;i<flen;i++)tx_ntb[off+i]=frame[i];return total;
}
static void notify_connection(BOOL up)
{
    ULONG bus;

    /* Linux cdc_ncm only publishes carrier after the data interface is in
     * alt-setting 1. Defer/retry link-up until SET_INTERFACE(1,1) has
     * opened the bulk endpoints, matching the validated CPU3 backend. */
    if(!usb_configured||notify_busy)return;
    if(up){
        if(usb_data_alt!=1||network_notification_sent)return;
    }else{
        if(usb_data_alt!=1)return;
    }

    notify_dma[0]=0xa1;notify_dma[1]=0x00;put_le16(notify_dma+2,up?1:0);put_le16(notify_dma+4,0);put_le16(notify_dma+6,0);
    if(notify_dma_active){dma_end(notify_dma,&notify_dma_len,DMA_READ_FROM_RAM);notify_dma_active=0;}
    bus=dma_begin(notify_dma,&notify_dma_len,8,DMA_READ_FROM_RAM);if(!bus)return;
    notify_dma_active=1;notify_busy=1;
    wr(USB_DIEPDMA(USB_NET_EP_NOTIFY),bus);
    wr(USB_DIEPTSIZ(USB_NET_EP_NOTIFY),USB_DXEPTSIZ_PKTCNT(1)|USB_DXEPTSIZ_XFERSIZE(8));
    wr(USB_DIEPCTL(USB_NET_EP_NOTIFY),rd(USB_DIEPCTL(USB_NET_EP_NOTIFY))|USB_DXEPCTL_EPENA|USB_DXEPCTL_CNAK);
    network_notification_sent=up?1:0;
}

static void data_endpoints_close(void)
{
    if(usb_data_alt)notify_connection(FALSE);
    usb_data_alt=0;bulk_in_state=BULK_IN_IDLE;bulk_tx_len=bulk_tx_pos=0;bulk_out_len=0;usb_rx_fast_prod=usb_rx_fast_cons=0;tx_slot_owned=0;tx_prod=tx_cons=0;
    wr(USB_DIEPCTL(USB_NET_EP_IN),rd(USB_DIEPCTL(USB_NET_EP_IN))|USB_DXEPCTL_SNAK);
    wr(USB_DOEPCTL(USB_NET_EP_OUT),rd(USB_DOEPCTL(USB_NET_EP_OUT))|USB_DXEPCTL_SNAK);
}
static void data_endpoints_open(void)
{
    usb_data_alt=1;network_notification_sent=0;bulk_in_state=BULK_IN_IDLE;bulk_tx_len=bulk_tx_pos=0;bulk_out_len=0;usb_rx_fast_prod=usb_rx_fast_cons=0;bulk_out_dma_index=0;tx_slot_owned=0;tx_prod=tx_cons=0;
    wr(USB_DIEPINT(USB_NET_EP_IN),0xffffffffUL);wr(USB_DOEPINT(USB_NET_EP_OUT),0xffffffffUL);
    wr(USB_DIEPCTL(USB_NET_EP_IN),USB_DXEPCTL_MPS(USB_NET_BULK_MPS)|USB_DXEPCTL_USBACTEP|USB_DXEPCTL_EPTYPE_BULK|USB_DXEPCTL_TXFNUM(USB_NET_EP_IN));
    wr(USB_DOEPCTL(USB_NET_EP_OUT),USB_DXEPCTL_MPS(USB_NET_BULK_MPS)|USB_DXEPCTL_USBACTEP|USB_DXEPCTL_EPTYPE_BULK);
    bulk_out_arm();
}
static void notify_endpoint_open(void)
{
    wr(USB_DIEPINT(USB_NET_EP_NOTIFY),0xffffffffUL);
    wr(USB_DIEPCTL(USB_NET_EP_NOTIFY),USB_DXEPCTL_MPS(USB_NET_NOTIFY_MPS)|USB_DXEPCTL_USBACTEP|USB_DXEPCTL_EPTYPE_INTR|USB_DXEPCTL_TXFNUM(USB_NET_EP_NOTIFY));
}

static void reset_state(void)
{
    usb_configured=0;usb_data_alt=0;bulk_in_state=BULK_IN_IDLE;notify_busy=0;network_notification_sent=0;bulk_out_len=0;usb_rx_fast_prod=usb_rx_fast_cons=0;bulk_out_dma_index=0;bulk_tx_len=bulk_tx_pos=0;tx_slot_owned=0;tx_prod=tx_cons=0;rx_prod=rx_cons=0;ncm_tx_seq=0;ep0_state=EP0_IDLE;ep0_out_kind=EP0_OUT_NONE;ep0_out_expected=0;ep0_tx_len=ep0_tx_pos=0;ncm_input_size=USB_NET_NTB_MAX;ncm_max_datagram=USB_NET_MAX_DATAGRAM;
}
static void dma_cancel_all(void)
{
    if(ep0_setup_dma_active){dma_end(ep0_setup_dma,&ep0_setup_dma_len,0);ep0_setup_dma_active=0;}
    if(ep0_out_dma_active){dma_end(ep0_out_dma,&ep0_out_dma_len,0);ep0_out_dma_active=0;}
    if(ep0_tx_dma_active){dma_end(ep0_tx_dma,&ep0_tx_dma_len,DMA_READ_FROM_RAM);ep0_tx_dma_active=0;}
    if(notify_dma_active){dma_end(notify_dma,&notify_dma_len,DMA_READ_FROM_RAM);notify_dma_active=0;}
    if(bulk_out_dma_active[0]){dma_end(bulk_out_dma[0],&bulk_out_dma_len[0],0);bulk_out_dma_active[0]=0;}
    if(bulk_out_dma_active[1]){dma_end(bulk_out_dma[1],&bulk_out_dma_len[1],0);bulk_out_dma_active[1]=0;}
    if(bulk_in_dma_active){dma_end(bulk_in_dma,&bulk_in_dma_len,DMA_READ_FROM_RAM);bulk_in_dma_active=0;}
}
static void on_reset(void)
{
    dma_cancel_all();reset_state();set_address_now(0);flush_fifos();
    wr(USB_DIEPINT(0),0xffffffffUL);wr(USB_DOEPINT(0),0xffffffffUL);
    wr(USB_DAINTMSK,USB_DAINT_INEP(0)|USB_DAINT_OUTEP(0)|USB_DAINT_INEP(USB_NET_EP_IN)|USB_DAINT_OUTEP(USB_NET_EP_OUT)|USB_DAINT_INEP(USB_NET_EP_NOTIFY));
    ep0_arm_setup();
}
static void on_enum_done(void){ep0_arm_setup();}

static void handle_setup(const struct usb_setup_packet *r)
{
    const UBYTE *data=NULL;UWORD len=0;UBYTE tmp[64];static const UBYTE zero2[2]={0,0};
    if((r->bmRequestType&0x80)&&r->bRequest==USB_REQ_GET_DESCRIPTOR){
        UBYTE type=(UBYTE)(r->wValue>>8),idx=(UBYTE)r->wValue;
        if(type==USB_DT_DEVICE){data=device_desc;len=sizeof(device_desc);}else if(type==USB_DT_DEVICE_QUALIFIER){data=qualifier_desc;len=sizeof(qualifier_desc);}else if(type==USB_DT_CONFIG){data=config_desc;len=sizeof(config_desc);}else if(type==USB_DT_STRING){
            if(idx==0){data=str0;len=sizeof(str0);}else if(idx==1){data=str1;len=sizeof(str1);}else if(idx==2){data=str2;len=sizeof(str2);}else if(idx==3){data=str3;len=sizeof(str3);}else if(idx==4){data=str4;len=sizeof(str4);}else if(idx==MS_OS_STRING_INDEX){data=ms_os_string_desc;len=sizeof(ms_os_string_desc);}
        }
        if(!data){ep0_stall();return;}if(len>r->wLength)len=r->wLength;ep0_state=EP0_IN_DATA;ep0_send(data,len);return;
    }
    if(r->bmRequestType==0x00&&r->bRequest==USB_REQ_SET_ADDRESS){set_address_now((UBYTE)(r->wValue&0x7f));ep0_state=EP0_IN_STATUS;ep0_zlp();return;}
    if(r->bmRequestType==0x00&&r->bRequest==USB_REQ_SET_CONFIGURATION){data_endpoints_close();usb_configured=r->wValue?1:0;network_notification_sent=0;if(usb_configured)notify_endpoint_open();ep0_state=EP0_IN_STATUS;ep0_zlp();return;}
    if(r->bmRequestType==0x80&&r->bRequest==USB_REQ_GET_CONFIGURATION){tmp[0]=usb_configured?1:0;ep0_state=EP0_IN_DATA;ep0_send(tmp,r->wLength<1?r->wLength:1);return;}
    if((r->bmRequestType&0x7f)==0x01&&r->bRequest==USB_REQ_SET_INTERFACE){UBYTE intf=(UBYTE)r->wIndex,alt=(UBYTE)r->wValue;if(intf==0&&alt==0){ep0_state=EP0_IN_STATUS;ep0_zlp();return;}if(intf==1&&usb_configured&&(alt==0||alt==1)){if(alt)data_endpoints_open();else data_endpoints_close();ep0_state=EP0_IN_STATUS;ep0_zlp();return;}}
    if((r->bmRequestType&0x7f)==0x01&&r->bRequest==USB_REQ_GET_INTERFACE){UBYTE intf=(UBYTE)r->wIndex;if(intf<=1){tmp[0]=intf==1?usb_data_alt:0;ep0_state=EP0_IN_DATA;ep0_send(tmp,r->wLength<1?r->wLength:1);return;}}
    if((r->bmRequestType&0x7f)==0x00&&r->bRequest==USB_REQ_GET_STATUS){ep0_state=EP0_IN_DATA;ep0_send(zero2,r->wLength<2?r->wLength:2);return;}
    if((r->bmRequestType&0x7f)==0x02&&r->bRequest==USB_REQ_CLEAR_FEATURE&&r->wValue==0){ep0_state=EP0_IN_STATUS;ep0_zlp();return;}
    if(r->bmRequestType==0xc0&&r->bRequest==MS_OS_VENDOR_CODE&&r->wIndex==MS_OS_EXT_COMPAT_ID_INDEX){len=sizeof(ms_os_ext_compat_id);if(len>r->wLength)len=r->wLength;ep0_state=EP0_IN_DATA;ep0_send(ms_os_ext_compat_id,len);return;}
    if((r->bmRequestType&0x1f)==0x01&&(UBYTE)r->wIndex==0){
        if(r->bmRequestType==0x21&&r->bRequest==CDC_SET_ETHERNET_PACKET_FILTER&&r->wLength==0){ep0_state=EP0_IN_STATUS;ep0_zlp();return;}
        if(r->bmRequestType==0xa1&&r->bRequest==NCM_GET_NTB_PARAMETERS){UWORD i;for(i=0;i<28;i++)tmp[i]=0;put_le16(tmp,28);put_le16(tmp+2,1);put_le32(tmp+4,USB_NET_NTB_MAX);put_le16(tmp+8,4);put_le16(tmp+12,4);put_le32(tmp+16,USB_NET_NTB_MAX);put_le16(tmp+20,4);put_le16(tmp+24,4);put_le16(tmp+26,1);ep0_state=EP0_IN_DATA;ep0_send(tmp,r->wLength<28?r->wLength:28);return;}
        if(r->bmRequestType==0xa1&&r->bRequest==NCM_GET_NTB_FORMAT){put_le16(tmp,0);ep0_state=EP0_IN_DATA;ep0_send(tmp,r->wLength<2?r->wLength:2);return;}
        if(r->bmRequestType==0x21&&r->bRequest==NCM_SET_NTB_FORMAT&&r->wValue==0){ep0_state=EP0_IN_STATUS;ep0_zlp();return;}
        if(r->bmRequestType==0xa1&&r->bRequest==NCM_GET_NTB_INPUT_SIZE){put_le32(tmp,ncm_input_size);ep0_state=EP0_IN_DATA;ep0_send(tmp,r->wLength<4?r->wLength:4);return;}
        if(r->bmRequestType==0x21&&r->bRequest==NCM_SET_NTB_INPUT_SIZE&&r->wLength==4){ep0_out_kind=EP0_OUT_NTB_INPUT_SIZE;ep0_out_expected=4;ep0_state=EP0_OUT_DATA;ep0_arm_out(4);return;}
        if(r->bmRequestType==0xa1&&r->bRequest==NCM_GET_MAX_DATAGRAM_SIZE){put_le16(tmp,ncm_max_datagram);ep0_state=EP0_IN_DATA;ep0_send(tmp,r->wLength<2?r->wLength:2);return;}
        if(r->bmRequestType==0x21&&r->bRequest==NCM_SET_MAX_DATAGRAM_SIZE&&r->wLength==2){ep0_out_kind=EP0_OUT_MAX_DATAGRAM;ep0_out_expected=2;ep0_state=EP0_OUT_DATA;ep0_arm_out(2);return;}
        if(r->bmRequestType==0x21&&r->bRequest==NCM_SET_CRC_MODE&&r->wValue==0){ep0_state=EP0_IN_STATUS;ep0_zlp();return;}
    }
    ep0_stall();
}

static void complete_ep0_out_data(const UBYTE *d,UWORD len)
{
    if(ep0_out_kind==EP0_OUT_NTB_INPUT_SIZE&&len==4){ULONG v=(ULONG)d[0]|((ULONG)d[1]<<8)|((ULONG)d[2]<<16)|((ULONG)d[3]<<24);if(v>=512&&v<=USB_NET_NTB_MAX)ncm_input_size=v;}
    else if(ep0_out_kind==EP0_OUT_MAX_DATAGRAM&&len==2){UWORD v=(UWORD)(d[0]|((UWORD)d[1]<<8));if(v>=576&&v<=USB_NET_MAX_DATAGRAM)ncm_max_datagram=v;}
    ep0_out_kind=EP0_OUT_NONE;ep0_out_expected=0;ep0_state=EP0_IN_STATUS;ep0_zlp();
}

static void poll_rx(void) { /* RX FIFO is bypassed in buffer-DMA mode. */ }
static void poll_epints(void)
{
    ULONG daint=rd(USB_DAINT);
    if(daint&USB_DAINT_OUTEP(0)){
        ULONG i=rd(USB_DOEPINT(0));if(i)wr(USB_DOEPINT(0),i);
        if(i&USB_DXEPINT_SETUP){
            struct usb_setup_packet r;
            if(ep0_setup_dma_active){dma_end(ep0_setup_dma,&ep0_setup_dma_len,0);ep0_setup_dma_active=0;}
            r.bmRequestType=ep0_setup_dma[0];r.bRequest=ep0_setup_dma[1];
            r.wValue=get_le16(ep0_setup_dma+2);r.wIndex=get_le16(ep0_setup_dma+4);r.wLength=get_le16(ep0_setup_dma+6);
            handle_setup(&r);
        }
        if(i&USB_DXEPINT_XFERCOMPL){
            if(ep0_state==EP0_OUT_STATUS){if(ep0_out_dma_active){dma_end(ep0_out_dma,&ep0_out_dma_len,0);ep0_out_dma_active=0;}ep0_state=EP0_IDLE;ep0_arm_setup();}
            else if(ep0_state==EP0_OUT_DATA){UWORD got=ep0_out_expected;if(ep0_out_dma_active){dma_end(ep0_out_dma,&ep0_out_dma_len,0);ep0_out_dma_active=0;}complete_ep0_out_data(ep0_out_dma,got);}
        }
    }
    if(daint&USB_DAINT_INEP(0)){
        ULONG i=rd(USB_DIEPINT(0));if(i)wr(USB_DIEPINT(0),i);
        if(i&USB_DXEPINT_XFERCOMPL){
            if(ep0_tx_dma_active){dma_end(ep0_tx_dma,&ep0_tx_dma_len,DMA_READ_FROM_RAM);ep0_tx_dma_active=0;}
            if(ep0_state==EP0_IN_STATUS){ep0_state=EP0_IDLE;ep0_arm_setup();}
            else if(ep0_state==EP0_IN_DATA){if(ep0_tx_pos<ep0_tx_len)ep0_send_next_chunk();else{ep0_state=EP0_OUT_STATUS;ep0_arm_out(0);}}
        }
    }
    if(daint&USB_DAINT_OUTEP(USB_NET_EP_OUT)){
        ULONG i=rd(USB_DOEPINT(USB_NET_EP_OUT));if(i)wr(USB_DOEPINT(USB_NET_EP_OUT),i);
        if(i&USB_DXEPINT_XFERCOMPL){
            const UBYTE done=0;
            ULONG residual=rd(USB_DOEPTSIZ(USB_NET_EP_OUT))&0x7ffffUL;
            UWORD got=(UWORD)(residual<=USB_NET_BULK_MPS?(USB_NET_BULK_MPS-residual):0);

            /* Single-DMA + software-staging diagnostic:
             *  1. end DMA ownership of the one known-good OUT buffer;
             *  2. copy the completed USB packet into the software NCM accumulator;
             *  3. immediately return the SAME DMA buffer to DWC2;
             *  4. only then do the heavier NCM parsing/SANA-II delivery work.
             *
             * This preserves the hardware ownership discipline of the validated
             * single-buffer path while overlapping the next OUT transfer with
             * software processing of data that has already been staged elsewhere. */
            if(bulk_out_dma_active[0]){dma_end(bulk_out_dma[0],&bulk_out_dma_len[0],0);bulk_out_dma_active[0]=0;}

            if(usb_configured&&usb_data_alt){
                UWORD j;
                BOOL staged=FALSE;

                if((ULONG)bulk_out_len+got<=sizeof(bulk_out_buf)){
                    for(j=0;j<got;j++)bulk_out_buf[bulk_out_len+j]=bulk_out_dma[done][j];
                    bulk_out_len=(UWORD)(bulk_out_len+got);
                    staged=TRUE;
                }else{
                    bulk_out_len=0;
                    stats.BadData++;
                }

                /* Critical difference from the validated serial single-buffer
                 * baseline: DWC2 gets slot 0 back BEFORE NCM/SANA processing. */
                (void)bulk_out_arm_index(0);

                if(staged)process_bulk_ntbs_ready();
            }
        }
    }
    if(daint&USB_DAINT_INEP(USB_NET_EP_IN)){
        ULONG i=rd(USB_DIEPINT(USB_NET_EP_IN));if(i)wr(USB_DIEPINT(USB_NET_EP_IN),i);
        if(i&USB_DXEPINT_XFERCOMPL){
            if(bulk_in_dma_active){dma_end(bulk_in_dma,&bulk_in_dma_len,DMA_READ_FROM_RAM);bulk_in_dma_active=0;}
            if(bulk_in_state==BULK_IN_DATA){
                if(bulk_tx_len&&((bulk_tx_len%USB_NET_BULK_MPS)==0)){
                    ULONG bus=dma_begin(bulk_in_dma,&bulk_in_dma_len,1,DMA_READ_FROM_RAM);
                    bulk_in_state=BULK_IN_ZLP;
                    if(bus){bulk_in_dma_active=1;wr(USB_DIEPDMA(USB_NET_EP_IN),bus);wr(USB_DIEPTSIZ(USB_NET_EP_IN),USB_DXEPTSIZ_PKTCNT(1)|USB_DXEPTSIZ_XFERSIZE(0));wr(USB_DIEPCTL(USB_NET_EP_IN),USB_DXEPCTL_MPS(USB_NET_BULK_MPS)|USB_DXEPCTL_USBACTEP|USB_DXEPCTL_EPTYPE_BULK|USB_DXEPCTL_TXFNUM(USB_NET_EP_IN)|USB_DXEPCTL_EPENA|USB_DXEPCTL_CNAK);}
                }else bulk_in_complete_logical();
            }else if(bulk_in_state==BULK_IN_ZLP)bulk_in_complete_logical();
        }
    }
    if(daint&USB_DAINT_INEP(USB_NET_EP_NOTIFY)){
        ULONG i=rd(USB_DIEPINT(USB_NET_EP_NOTIFY));if(i)wr(USB_DIEPINT(USB_NET_EP_NOTIFY),i);
        if(i&USB_DXEPINT_XFERCOMPL){if(notify_dma_active){dma_end(notify_dma,&notify_dma_len,DMA_READ_FROM_RAM);notify_dma_active=0;}notify_busy=0;}
    }
}
static void usb_poll(void)
{
    ULONG g;if(!hw_ready)return;g=rd(USB_GINTSTS);
    if(g&USB_GINTSTS_USBRST){wr(USB_GINTSTS,USB_GINTSTS_USBRST);on_reset();}
    if(g&USB_GINTSTS_ENUMDONE){wr(USB_GINTSTS,USB_GINTSTS_ENUMDONE);on_enum_done();}
    if(g&(USB_GINTSTS_IEPINT|USB_GINTSTS_OEPINT))poll_epints();
    bulk_in_fill_fifo();
    if(usb_configured&&usb_data_alt==1&&online&&!notify_busy&&!network_notification_sent)notify_connection(TRUE);
    tx_kick();
}

/* Bounded task-context drain.  This is intentionally not a free-running poll
 * loop: service only work already visible at the controller, then return to
 * Wait().  The bound protects Exec fairness and keeps this structure directly
 * replaceable by an IRQ wakeup. */
#define UNIT_USB_SERVICE_BUDGET 8
static void unit_usb_service(void)
{
    int budget=UNIT_USB_SERVICE_BUDGET;
    while(budget-- > 0){
        ULONG before,after;
        if(!hw_ready)break;
        before=rd(USB_GINTSTS)&USB_GINTMSK_USBNET;
        usb_poll();
        after=rd(USB_GINTSTS)&USB_GINTMSK_USBNET;
        if(!after || after==before)break;
    }

    /* The level-6 ISR masks the DWC2 global gate, fast-services EP1 OUT, and
     * wakes us.  Re-open the gate only after the bounded task-context drain.
     * If new status became pending meanwhile, DWC2 immediately asserts IRQ. */
    if(dwc2_irq_added && hw_ready)
        wr(USB_GAHBCFG,rd(USB_GAHBCFG)|USB_GAHBCFG_GLBL_INTR_EN);
}

static int hw_init(void)
{
    ULONG v;if(hw_ready)return 1;
    if(!mailbox_power_usb()) return 0;
    if(DOSBase) Delay(1);
    v=rd(USB_GSNPSID);if((v&0xffff0000UL)!=0x4f540000UL)return 0;
    wr(USB_DCTL,rd(USB_DCTL)|USB_DCTL_SFTDISCON);
    v=rd(USB_GUSBCFG);v&=~(USB_GUSBCFG_FORCEHOSTMODE|USB_GUSBCFG_HNPCAP|USB_GUSBCFG_SRPCAP|USB_GUSBCFG_TOUTCAL_MASK);v|=USB_GUSBCFG_FORCEDEVMODE|7;wr(USB_GUSBCFG,v);
    if(DOSBase)Delay(2);
    if(!wait_mask(USB_GRSTCTL,USB_GRSTCTL_AHBIDLE,USB_GRSTCTL_AHBIDLE,5000000UL))return 0;
    wr(USB_GRSTCTL,USB_GRSTCTL_CSFTRST);if(!wait_mask(USB_GRSTCTL,USB_GRSTCTL_CSFTRST,0,5000000UL))return 0;
    if(DOSBase)Delay(1);
    v=rd(USB_GUSBCFG);v&=~USB_GUSBCFG_FORCEHOSTMODE;v|=USB_GUSBCFG_FORCEDEVMODE;wr(USB_GUSBCFG,v);
    if(DOSBase)Delay(2);if(rd(USB_GINTSTS)&USB_GINTSTS_CURMODE_HOST)return 0;
    v=rd(USB_GAHBCFG);v&=~USB_GAHBCFG_GLBL_INTR_EN;v|=USB_GAHBCFG_DMA_EN;wr(USB_GAHBCFG,v);wr(USB_GINTMSK,0);
    wr(USB_GRXFSIZ,256);wr(USB_GNPTXFSIZ,(128UL<<16)|256);wr(USB_DPTXFSIZ(USB_NET_EP_IN),(512UL<<16)|384);wr(USB_DPTXFSIZ(USB_NET_EP_NOTIFY),(32UL<<16)|896);flush_fifos();
    v=rd(USB_DCFG);v&=~(USB_DCFG_DEVADDR_MASK|USB_DCFG_DEVSPD_MASK);wr(USB_DCFG,v);
    wr(USB_DIEPMSK,USB_DXEPINT_XFERCOMPL);wr(USB_DOEPMSK,USB_DXEPINT_XFERCOMPL|USB_DXEPINT_SETUP);
    hw_ready=1;on_reset();wr(USB_DCTL,rd(USB_DCTL)&~USB_DCTL_SFTDISCON);return 1;
}
static void hw_stop(void)
{
    if(hw_ready){
        wr(USB_GAHBCFG,rd(USB_GAHBCFG)&~USB_GAHBCFG_GLBL_INTR_EN);
        wr(USB_GINTMSK,0);
        bcm_irq_write(BCM_IRQ_DISABLE1_ADDR,BCM_IRQ_USB_BIT);
        wr(USB_DCTL,rd(USB_DCTL)|USB_DCTL_SFTDISCON);
        dma_cancel_all();
        hw_ready=0;
    }
}

/* Handle only EP1 OUT XFERCOMPL at IPL6.  No NCM parser, Exec message
 * completion or SANA-II callback runs here.  The packet is copied into an
 * SPSC handoff ring and the SAME single DMA buffer is immediately re-armed. */
static BOOL dwc2_irq_fast_ep1_out(void)
{
    ULONG daint,i,residual;
    UWORD got,prod,next,j;

    daint=rd(USB_DAINT);
    if(!(daint&USB_DAINT_OUTEP(USB_NET_EP_OUT)))return FALSE;

    i=rd(USB_DOEPINT(USB_NET_EP_OUT));
    if(!(i&USB_DXEPINT_XFERCOMPL))return FALSE;
    /* Consume only XFERCOMPL here; leave any other endpoint causes for UnitTask. */
    wr(USB_DOEPINT(USB_NET_EP_OUT),USB_DXEPINT_XFERCOMPL);

    residual=rd(USB_DOEPTSIZ(USB_NET_EP_OUT))&0x7ffffUL;
    got=(UWORD)(residual<=USB_NET_BULK_MPS?(USB_NET_BULK_MPS-residual):0);

    if(bulk_out_dma_active[0]){
        dma_end(bulk_out_dma[0],&bulk_out_dma_len[0],0);
        bulk_out_dma_active[0]=0;
    }

    if(usb_configured&&usb_data_alt){
        prod=usb_rx_fast_prod;
        next=(UWORD)((prod+1U)&USB_RX_FAST_MASK);
        if(next!=usb_rx_fast_cons && got<=USB_NET_BULK_MPS){
            usb_rx_fast_ring[prod].len=got;
            for(j=0;j<got;j++)usb_rx_fast_ring[prod].data[j]=bulk_out_dma[0][j];
            __asm__ volatile("" ::: "memory");
            usb_rx_fast_prod=next;
            __asm__ volatile("" ::: "memory");
        }else{
            usb_rx_fast_drops++;
        }

        /* Preserve the validated packet-granular transaction: 512 bytes,
         * PKTCNT=1, slot 0 as the only DMA owner. */
        (void)bulk_out_arm_index(0);
    }
    return TRUE;
}

/* Emu68 on Pi3 converts ARM IRQs delivered to core 0 into the Amiga EXTER
 * interrupt (IPL6).  EP1 OUT gets a minimal hardware fast path; all NCM and
 * SANA-II work remains in UnitTask. */
static ULONG __attribute__((used)) dwc2_irq_handler(APTR data asm("a1"))
{
    ULONG pending;
    (void)data;

    if(!hw_ready || !dwc2_irq_added)return 0;
    /* EXTER is shared: claim only when the BCM legacy controller says the
     * USB source itself is pending, then confirm a masked-in DWC2 cause. */
    if(!(bswap32(mmio_raw_read(BCM_IRQ_PENDING1_ADDR))&BCM_IRQ_USB_BIT))return 0;
    pending=rd(USB_GINTSTS)&rd(USB_GINTMSK)&USB_GINTMSK_USBNET;
    if(!pending)return 0;

    /* Quiesce the shared level source first.  EP1 OUT completion is then
     * consumed immediately so the same DMA buffer can be re-armed without
     * waiting for UnitTask scheduling.  Other USB causes remain pending for
     * the existing task-context service. */
    wr(USB_GAHBCFG,rd(USB_GAHBCFG)&~USB_GAHBCFG_GLBL_INTR_EN);
    (void)dwc2_irq_fast_ep1_out();
    if(unit_task_ready && unit_task && unit_usb_sigmask)
        Signal(unit_task,unit_usb_sigmask);

    /* Clear Emu68's synthetic EXTER latch.  The real DWC2 source is already
     * gated above and will be re-enabled by UnitTask after it is drained. */
    *(volatile UWORD *)0xDFF09CUL=(UWORD)0x2000U;
    return 1;
}

static void dwc2_irq_install(void)
{
    ULONG v;
    if(dwc2_irq_added || !hw_ready || !unit_task_ready || !unit_task)return;

    /* Keep DWC2 gated until the Exec server and BCM source are both ready. */
    wr(USB_GAHBCFG,rd(USB_GAHBCFG)&~USB_GAHBCFG_GLBL_INTR_EN);
    wr(USB_GINTMSK,0);

    dwc2_irq_int.is_Node.ln_Type=NT_INTERRUPT;
    dwc2_irq_int.is_Node.ln_Pri=5;
    dwc2_irq_int.is_Node.ln_Name=(STRPTR)DEVICE_NAME;
    dwc2_irq_int.is_Data=NULL;
    dwc2_irq_int.is_Code=(void (*)())dwc2_irq_handler;
    AddIntServer(INTB_EXTER,&dwc2_irq_int);
    dwc2_irq_added=TRUE;

    wr(USB_GINTMSK,USB_GINTMSK_USBNET);
    bcm_irq_write(BCM_IRQ_ENABLE1_ADDR,BCM_IRQ_USB_BIT);
    v=rd(USB_GAHBCFG);
    wr(USB_GAHBCFG,v|USB_GAHBCFG_GLBL_INTR_EN);
}

static void dwc2_irq_remove(void)
{
    if(hw_ready){
        wr(USB_GAHBCFG,rd(USB_GAHBCFG)&~USB_GAHBCFG_GLBL_INTR_EN);
        wr(USB_GINTMSK,0);
    }
    bcm_irq_write(BCM_IRQ_DISABLE1_ADDR,BCM_IRQ_USB_BIT);
    if(dwc2_irq_added){
        dwc2_irq_added=FALSE;
        RemIntServer(INTB_EXTER,&dwc2_irq_int);
    }
}

static ULONG __attribute__((used)) vblank_handler(APTR data asm("a1"))
{
    (void)data;
    /* POC2N beta watchdog/fallback only.  DWC2 IRQ is the primary event
     * source; this low-rate wakeup never touches controller or SANA-II state. */
    if(unit_task_ready && unit_task && unit_usb_sigmask)
        Signal(unit_task,unit_usb_sigmask);
    return 0;
}
static void vblank_install(void)
{
    if(vblank_added)return;
    vblank_int.is_Node.ln_Type=NT_INTERRUPT;vblank_int.is_Node.ln_Pri=0;vblank_int.is_Node.ln_Name=(STRPTR)DEVICE_NAME;vblank_int.is_Data=NULL;vblank_int.is_Code=(void (*)())vblank_handler;AddIntServer(INTB_VERTB,&vblank_int);vblank_added=TRUE;
}
static void vblank_remove(void){if(vblank_added){RemIntServer(INTB_VERTB,&vblank_int);vblank_added=FALSE;}}

static BOOL parse_open_tags(struct TagItem *t)
{
    copy_to_buff=NULL;copy_from_buff=NULL;while(t){ULONG tag=t->ti_Tag;if(tag==TAG_DONE)break;if(tag==TAG_IGNORE){t++;continue;}if(tag==TAG_MORE){t=(struct TagItem *)t->ti_Data;continue;}if(tag==TAG_SKIP){t+=(ULONG)t->ti_Data+1;continue;}if(tag==S2_CopyToBuff)copy_to_buff=(CopyFunc)t->ti_Data;else if(tag==S2_CopyFromBuff)copy_from_buff=(CopyFunc)t->ti_Data;t++;}return copy_to_buff&&copy_from_buff;
}
static BOOL call_copy(CopyFunc fn,APTR to,APTR from,ULONG len)
{
    register APTR ra0 asm("a0")=to;register APTR ra1 asm("a1")=from;register ULONG rd0 asm("d0")=len;register APTR ra2 asm("a2")=fn;if(!fn)return FALSE;
    __asm__ volatile("jsr (%3)":"+r"(rd0):"r"(ra0),"r"(ra1),"a"(ra2):"d1","a0","a1","cc","memory");return rd0?TRUE:FALSE;
}
static void queue_read(struct IOSana2Req *io)
{
    /* Called only by UnitTask. */
    io->ios2_Req.io_Flags|=PENDING_BIT;
    io->ios2_Req.io_Flags&=~IOF_QUICK;
    AddTail(&read_q,&io->ios2_Req.io_Message.mn_Node);
}
static BOOL tx_ring_has_space(void)
{
    UWORD next=(UWORD)((tx_prod+1U)&TX_RING_MASK);
    return next!=tx_cons;
}

/*
 * Return TRUE when the request has reached a terminal state and may be
 * replied now.  Return FALSE when transient TX congestion has moved the
 * request to tx_wait_q; its caller buffer remains owned by the request until
 * a later bulk-IN completion frees a ring slot.
 */
static BOOL do_write(struct IOSana2Req *io,BOOL bcast)
{
    ULONG len,flen,type;UWORD ntblen;int i;
    io->ios2_Req.io_Error=0;io->ios2_WireError=0;
    if(!online||!usb_configured||!usb_data_alt){io->ios2_Req.io_Error=S2ERR_OUTOFSERVICE;io->ios2_WireError=S2WERR_UNIT_OFFLINE;return TRUE;}

    /* Backpressure before touching the caller buffer.  A full private NTB
       ring is transient congestion, not a SANA-II transmit error. */
    if(!tx_ring_has_space()){
        io->ios2_Req.io_Flags|=PENDING_BIT;
        io->ios2_Req.io_Flags&=~IOF_QUICK;
        AddTail(&tx_wait_q,&io->ios2_Req.io_Message.mn_Node);
        return FALSE;
    }

    ObtainSemaphore(&tx_sem);
    if(io->ios2_Req.io_Flags&SANA2IOF_RAW){
        len=io->ios2_DataLength;
        if(len>USB_NET_MAX_DATAGRAM||!call_copy(copy_from_buff,tx_frame,io->ios2_Data,len)){
            io->ios2_Req.io_Error=(len>USB_NET_MAX_DATAGRAM)?S2ERR_MTU_EXCEEDED:S2ERR_NO_RESOURCES;
            io->ios2_WireError=S2WERR_BUFF_ERROR;ReleaseSemaphore(&tx_sem);return TRUE;
        }
        flen=len;
    }else{
        len=io->ios2_DataLength;
        if(len>ARMNET_ETH_MTU){io->ios2_Req.io_Error=S2ERR_MTU_EXCEEDED;ReleaseSemaphore(&tx_sem);return TRUE;}
        if(bcast)for(i=0;i<6;i++)tx_frame[i]=0xff;else mac_copy(tx_frame,io->ios2_DstAddr);
        mac_copy(tx_frame+6,amiga_mac);type=io->ios2_PacketType;tx_frame[12]=(UBYTE)(type>>8);tx_frame[13]=(UBYTE)type;
        if(!call_copy(copy_from_buff,tx_frame+ETH_HLEN,io->ios2_Data,len)){
            io->ios2_Req.io_Error=S2ERR_NO_RESOURCES;io->ios2_WireError=S2WERR_BUFF_ERROR;ReleaseSemaphore(&tx_sem);return TRUE;
        }
        flen=ETH_HLEN+len;
    }
    ntblen=ncm_build_ntb(tx_frame,(UWORD)flen);
    if(!ntblen){io->ios2_Req.io_Error=S2ERR_NO_RESOURCES;io->ios2_WireError=S2WERR_GENERIC_ERROR;}
    else if(!tx_enqueue_ntb(tx_ntb,ntblen)){
        /* UnitTask is the sole TX producer, so this should only be reachable
           if state changed unexpectedly. Preserve backpressure rather than
           surfacing a transient transport-full condition to Miami. */
        ReleaseSemaphore(&tx_sem);
        io->ios2_Req.io_Flags|=PENDING_BIT;io->ios2_Req.io_Flags&=~IOF_QUICK;
        AddTail(&tx_wait_q,&io->ios2_Req.io_Message.mn_Node);
        return FALSE;
    }else stats.PacketsSent++;
    ReleaseSemaphore(&tx_sem);
    return TRUE;
}

static void unit_drain_pending_writes(void)
{
    struct Node *n;
    while(tx_ring_has_space()&&(n=RemHead(&tx_wait_q))!=NULL){
        struct IOSana2Req *io=(struct IOSana2Req *)n;
        BOOL bcast=(io->ios2_Req.io_Command==S2_BROADCAST)?TRUE:FALSE;
        if(io->ios2_Req.io_Error==IOERR_ABORTED){unit_reply_aborted(io);continue;}
        if(do_write(io,bcast))finish_io(io);
        else break;
    }
}

static void unit_reply_aborted(struct IOSana2Req *io)
{
    io->ios2_Req.io_Error=IOERR_ABORTED;
    io->ios2_WireError=0;
    finish_io(io);
}

static void unit_abort_reads(void)
{
    struct Node *n=read_q.lh_Head;
    while(n->ln_Succ){
        struct Node *next=n->ln_Succ;
        struct IOSana2Req *io=(struct IOSana2Req *)n;
        if(io->ios2_Req.io_Error==IOERR_ABORTED){Remove(n);unit_reply_aborted(io);}
        n=next;
    }
    n=tx_wait_q.lh_Head;
    while(n->ln_Succ){
        struct Node *next=n->ln_Succ;
        struct IOSana2Req *io=(struct IOSana2Req *)n;
        if(io->ios2_Req.io_Error==IOERR_ABORTED){Remove(n);unit_reply_aborted(io);}
        n=next;
    }
}

static void unit_process_command(struct IOSana2Req *io)
{
    struct IORequest *req=&io->ios2_Req;

    if(req->io_Error==IOERR_ABORTED){unit_reply_aborted(io);return;}
    req->io_Error=0;io->ios2_WireError=0;

    if(io->ios2_BufferManagement!=opener_cookie){
        req->io_Error=S2ERR_BAD_ARGUMENT;io->ios2_WireError=S2WERR_BUFF_ERROR;
        finish_io(io);return;
    }

    switch(req->io_Command){
    case CMD_READ:case S2_READORPHAN:
        if(!online){req->io_Error=S2ERR_OUTOFSERVICE;io->ios2_WireError=S2WERR_UNIT_OFFLINE;break;}
        queue_read(io);return;
    case CMD_WRITE:if(!do_write(io,FALSE))return;break;
    case S2_MULTICAST:if(!do_write(io,FALSE))return;break;
    case S2_BROADCAST:if(!do_write(io,TRUE))return;break;
    case S2_DEVICEQUERY:
        if(io->ios2_StatData){struct Sana2DeviceQuery *q=(struct Sana2DeviceQuery *)io->ios2_StatData;ULONG avail=q->SizeAvailable;if(avail<sizeof(*q)){req->io_Error=S2ERR_BAD_ARGUMENT;io->ios2_WireError=S2WERR_BAD_STATDATA;break;}q->SizeSupplied=sizeof(*q);q->DevQueryFormat=0;q->DeviceLevel=0;q->AddrFieldSize=48;q->MTU=ARMNET_ETH_MTU;q->BPS=100000000UL;q->HardwareType=S2WireType_Ethernet;}else{req->io_Error=S2ERR_BAD_ARGUMENT;io->ios2_WireError=S2WERR_BAD_STATDATA;}break;
    case S2_GETSTATIONADDRESS:mac_copy(io->ios2_SrcAddr,amiga_mac);mac_copy(io->ios2_DstAddr,amiga_mac);break;
    case S2_CONFIGINTERFACE:if(configured){req->io_Error=S2ERR_BAD_STATE;io->ios2_WireError=S2WERR_IS_CONFIGURED;break;}configured=TRUE;online=TRUE;if(usb_data_alt){network_notification_sent=0;notify_connection(TRUE);}stats.Reconfigurations++;break;
    case S2_ONLINE:if(!configured){req->io_Error=S2ERR_BAD_STATE;io->ios2_WireError=S2WERR_NOT_CONFIGURED;break;}online=TRUE;if(usb_data_alt){network_notification_sent=0;notify_connection(TRUE);}break;
    case S2_OFFLINE:online=FALSE;if(usb_data_alt)notify_connection(FALSE);break;
    case S2_ADDMULTICASTADDRESS:case S2_DELMULTICASTADDRESS:case S2_TRACKTYPE:case S2_UNTRACKTYPE:break;
    case S2_GETGLOBALSTATS:if(io->ios2_StatData)CopyMem(&stats,io->ios2_StatData,sizeof(stats));else{req->io_Error=S2ERR_BAD_ARGUMENT;io->ios2_WireError=S2WERR_BAD_STATDATA;}break;
    case S2_GETTYPESTATS:if(io->ios2_StatData)zero_mem(io->ios2_StatData,sizeof(struct Sana2PacketTypeStats));else{req->io_Error=S2ERR_BAD_ARGUMENT;io->ios2_WireError=S2WERR_BAD_STATDATA;}break;
    case S2_GETSPECIALSTATS:if(io->ios2_StatData)((struct Sana2SpecialStatHeader *)io->ios2_StatData)->RecordCountSupplied=0;else{req->io_Error=S2ERR_BAD_ARGUMENT;io->ios2_WireError=S2WERR_BAD_STATDATA;}break;
    case NSCMD_DEVICEQUERY:{struct NSDeviceQueryResult *r=(struct NSDeviceQueryResult *)io->ios2_Data;if(!r||io->ios2_DataLength<sizeof(*r)){req->io_Error=IOERR_BADLENGTH;break;}r->DevQueryFormat=0;r->SizeAvailable=sizeof(*r);r->DeviceType=NSDEVTYPE_SANA2;r->DeviceSubType=0;r->SupportedCommands=supported_cmds;io->ios2_DataLength=sizeof(*r);break;}
    default:req->io_Error=IOERR_NOCMD;break;
    }
    finish_io(io);
}

static void unit_flush_all(void)
{
    struct Message *m;struct Node *n;
    if(unit_port){
        while((m=GetMsg(unit_port))!=NULL)unit_reply_aborted((struct IOSana2Req *)m);
    }
    while((n=RemHead(&read_q))!=NULL)unit_reply_aborted((struct IOSana2Req *)n);
    while((n=RemHead(&tx_wait_q))!=NULL)unit_reply_aborted((struct IOSana2Req *)n);
}

static void unit_task_entry(void)
{
    ULONG waitmask,sigs;BYTE msgbit=-1;int budget;struct Message *m;

    init_list(&read_q);
    init_list(&tx_wait_q);
    unit_task=FindTask(NULL);
    unit_port=CreateMsgPort();
    if(!unit_port)goto fail;
    msgbit=unit_port->mp_SigBit;

    unit_rx_sigbit=AllocSignal(-1);
    unit_abort_sigbit=AllocSignal(-1);
    unit_usb_sigbit=AllocSignal(-1);
    if(unit_rx_sigbit<0||unit_abort_sigbit<0||unit_usb_sigbit<0)goto fail;
    unit_rx_sigmask=1UL<<(UBYTE)unit_rx_sigbit;
    unit_abort_sigmask=1UL<<(UBYTE)unit_abort_sigbit;
    unit_usb_sigmask=1UL<<(UBYTE)unit_usb_sigbit;

    unit_task_ready=TRUE;
    if(unit_parent)Signal(unit_parent,SIGBREAKF_CTRL_F);

    waitmask=(1UL<<(UBYTE)msgbit)|unit_rx_sigmask|unit_abort_sigmask|unit_usb_sigmask|SIGBREAKF_CTRL_C;
    for(;;){
        sigs=Wait(waitmask);
        if((sigs&SIGBREAKF_CTRL_C)||unit_task_stopping)break;

        /* Temporary VBlank event source; final design will wake this same path
         * from the DWC2 IRQ.  All controller service now runs in task context. */
        if(sigs&unit_usb_sigmask){
            /* EP1 packets were already DMA-completed/staged/re-armed at IPL6.
             * Assemble/parse them here, then service every non-fast-path USB
             * cause using the original bounded task-context machinery. */
            usb_rx_fast_drain();
            unit_usb_service();
            unit_drain_pending_writes();
        }

        /* Abort marks are consumed before normal completion. */
        if(sigs&unit_abort_sigmask)unit_abort_reads();

        if(sigs&(1UL<<(UBYTE)msgbit)){
            budget=64;
            while(budget>0&&(m=GetMsg(unit_port))!=NULL){
                unit_process_command((struct IOSana2Req *)m);
                budget--;
            }
            if(budget==0)Signal(unit_task,1UL<<(UBYTE)msgbit);
            unit_drain_pending_writes();
        }

        if(sigs&unit_rx_sigmask){
            rx_drain();
            if(rx_cons!=rx_prod)Signal(unit_task,unit_rx_sigmask);
        }
    }

    unit_flush_all();
fail:
    unit_task_ready=FALSE;
    if(unit_port){DeleteMsgPort(unit_port);unit_port=NULL;}
    if(unit_usb_sigbit>=0){FreeSignal(unit_usb_sigbit);unit_usb_sigbit=-1;}
    if(unit_abort_sigbit>=0){FreeSignal(unit_abort_sigbit);unit_abort_sigbit=-1;}
    if(unit_rx_sigbit>=0){FreeSignal(unit_rx_sigbit);unit_rx_sigbit=-1;}
    unit_rx_sigmask=unit_abort_sigmask=unit_usb_sigmask=0;
    unit_task=NULL;unit_process=NULL;
    if(unit_parent)Signal(unit_parent,SIGBREAKF_CTRL_F);
}

/* The GENET package uses drv_task_spawn/join/exit, but that helper's source is
 * not present in the supplied archive.  For this self-contained POC we keep
 * the same UnitTask ownership/message/signal model and use DOS' process
 * creation only as the launcher.  No SANA-II work is done outside UnitTask. */
static BOOL unit_task_start(void)
{
    ULONG guard=200;
    if(unit_task_ready||unit_process)return TRUE;
    if(!DOSBase)return FALSE;

    unit_parent=FindTask(NULL);
    unit_task=NULL;unit_port=NULL;unit_task_ready=FALSE;unit_task_stopping=FALSE;
    unit_rx_sigbit=unit_abort_sigbit=unit_usb_sigbit=-1;unit_rx_sigmask=unit_abort_sigmask=unit_usb_sigmask=0;

    SetSignal(0,SIGBREAKF_CTRL_F);
    unit_process=CreateNewProcTags(
        NP_Entry,(ULONG)unit_task_entry,
        NP_Name,(ULONG)"usbnet.unit",
        NP_StackSize,UNIT_TASK_STACK,
        /* Keep RX completion in UnitTask until its bounded drain reaches Wait().
         * Replies remain immediate; priority +1 only prevents a normal-priority
         * Miami task from preempting us on every ReplyMsg(). */
        NP_Priority,1,
        TAG_DONE);
    if(!unit_process)return FALSE;

    while(!unit_task_ready&&unit_process&&guard--){
        if(SetSignal(0,0)&SIGBREAKF_CTRL_F)break;
        Delay(1);
    }
    return unit_task_ready&&unit_task&&unit_port;
}

static void unit_task_stop(void)
{
    ULONG guard=200;
    if(!unit_process&&!unit_task_ready)return;
    unit_task_stopping=TRUE;
    if(unit_task)Signal(unit_task,SIGBREAKF_CTRL_C);
    while((unit_task_ready||unit_process)&&guard--)Delay(1);
    unit_task_stopping=FALSE;
}


static BOOL dma_plumbing_selftest(void)
{
    ULONG len,phys;

    /* DWC2 -> RAM mapping (bulk OUT direction).  This does NOT enable DWC2 DMA. */
    len=DMA_TEST_SIZE;
    phys=(ULONG)CachePreDMA((APTR)dma_test_buf,&len,0);
    if(!phys || len<DMA_TEST_SIZE){
        if(phys) CachePostDMA((APTR)dma_test_buf,&len,0);
        return FALSE;
    }
    dma_test_bus_rx=(phys & 0x3fffffffUL) | 0xc0000000UL;
    CachePostDMA((APTR)dma_test_buf,&len,0);

    /* RAM -> DWC2 mapping (bulk IN direction), same convention as berrypi3ap.device. */
    len=DMA_TEST_SIZE;
    phys=(ULONG)CachePreDMA((APTR)dma_test_buf,&len,DMA_READ_FROM_RAM);
    if(!phys || len<DMA_TEST_SIZE){
        if(phys) CachePostDMA((APTR)dma_test_buf,&len,DMA_READ_FROM_RAM);
        return FALSE;
    }
    dma_test_bus_tx=(phys & 0x3fffffffUL) | 0xc0000000UL;
    CachePostDMA((APTR)dma_test_buf,&len,DMA_READ_FROM_RAM);

    return dma_test_bus_rx!=0 && dma_test_bus_tx!=0;
}

static BPTR do_expunge(struct Library *dev){if(dev->lib_OpenCnt){dev->lib_Flags|=LIBF_DELEXP;return 0;}return 0;}
static void do_open(struct Library *dev,struct IORequest *req,ULONG unitnum,ULONG flags)
{
    struct IOSana2Req *io=(struct IOSana2Req *)req;(void)flags;
    req->io_Error=IOERR_OPENFAIL;req->io_Message.mn_Node.ln_Type=NT_REPLYMSG;
    if(req->io_Message.mn_Length<(UWORD)sizeof(struct IOSana2Req)||unitnum!=0)return;
    if(is_open){req->io_Error=IOERR_UNITBUSY;return;}
    if(!io->ios2_BufferManagement||!parse_open_tags((struct TagItem *)io->ios2_BufferManagement))return;

    opener_cookie=io->ios2_BufferManagement;
    InitSemaphore(&tx_sem);zero_mem(&stats,sizeof(stats));
    configured=FALSE;online=FALSE;rx_prod=rx_cons=0;tx_prod=tx_cons=0;
    if(!DOSBase)DOSBase=(struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library",0);

    /* POC2M1: validate the exact Exec DMA mapping/coherency API proven by
       berrypi3ap.device before switching DWC2 into global buffer-DMA mode.
       EP0, bulk OUT/IN and notification all use DMA: no endpoint is left in
       the old FIFO datapath after GAHBCFG_DMA_EN is asserted. */
    if(!dma_plumbing_selftest()){
        copy_to_buff=copy_from_buff=NULL;opener_cookie=NULL;
        req->io_Error=IOERR_OPENFAIL;return;
    }

    /* Keep POC2J ownership intact.  Enumeration can start under the existing
     * VBlank watchdog, but true DWC2 IRQ is enabled only after UnitTask exists. */
    (void)hw_init();
    if(hw_ready)vblank_install();

    if(!unit_task_start()){
        vblank_remove();hw_stop();copy_to_buff=copy_from_buff=NULL;opener_cookie=NULL;
        req->io_Error=IOERR_OPENFAIL;return;
    }
    if(hw_ready)dwc2_irq_install();

    is_open=TRUE;req->io_Unit=(struct Unit *)dev;dev->lib_OpenCnt++;req->io_Error=0;
}

static BPTR do_close(struct Library *dev,struct IORequest *req)
{
    dwc2_irq_remove();
    vblank_remove();
    unit_task_stop();
    online=FALSE;
    hw_stop();
    configured=FALSE;is_open=FALSE;rx_prod=rx_cons=0;tx_prod=tx_cons=0;
    copy_to_buff=copy_from_buff=NULL;opener_cookie=NULL;
    req->io_Device=NULL;req->io_Unit=NULL;
    if(dev->lib_OpenCnt)dev->lib_OpenCnt--;
    return 0;
}

static void do_begin_io(struct Library *dev,struct IORequest *req)
{
    struct IOSana2Req *io=(struct IOSana2Req *)req;(void)dev;
    req->io_Message.mn_Node.ln_Type=NT_MESSAGE;
    req->io_Error=0;io->ios2_WireError=0;

    if(!unit_task_ready||!unit_port){
        req->io_Error=IOERR_OPENFAIL;
        if(!(req->io_Flags&IOF_QUICK))ReplyMsg(&req->io_Message);
        return;
    }

    /* GENET model: BeginIO is only a producer. UnitTask owns command execution,
       read_q, CopyTo/FromBuff callbacks and request completion. */
    req->io_Flags|=PENDING_BIT;
    req->io_Flags&=~IOF_QUICK;
    PutMsg(unit_port,&req->io_Message);
}

static ULONG do_abort_io(struct Library *dev,struct IORequest *req)
{
    struct IOSana2Req *io=(struct IOSana2Req *)req;(void)dev;
    if(!(req->io_Flags&PENDING_BIT))return IOERR_NOCMD;

    /* Do not touch unit_port or read_q here.  Mark the request; UnitTask is the
       sole queue owner and will retire it either while draining the MsgPort or
       from its private read_q on the abort signal. */
    req->io_Error=IOERR_ABORTED;
    io->ios2_WireError=0;
    if(unit_task_ready&&unit_task&&unit_abort_sigmask)Signal(unit_task,unit_abort_sigmask);
    return 0;
}

int __attribute__((no_reorder)) _start(void){return -1;}
asm("romtag:                                \n"
    "       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
    "       dc.l    romtag                  \n"
    "       dc.l    endcode                 \n"
    "       dc.b    "XSTR(RTF_AUTOINIT)"    \n"
    "       dc.b    "XSTR(DEVICE_VERSION)"  \n"
    "       dc.b    "XSTR(NT_DEVICE)"       \n"
    "       dc.b    "XSTR(DEVICE_PRIORITY)" \n"
    "       dc.l    _device_name            \n"
    "       dc.l    _device_id_string       \n"
    "       dc.l    _auto_init_tables       \n"
    "endcode:                               \n");
char device_name[]=DEVICE_NAME;char device_id_string[]=DEVICE_ID_STRING;
static struct Library __attribute__((used))*init_device(struct ExecBase *sys_base asm("a6"),BPTR seg_list asm("a0"),struct Library *dev asm("d0")){SysBase=sys_base;saved_seg_list=seg_list;dev->lib_Node.ln_Type=NT_DEVICE;dev->lib_Node.ln_Name=device_name;dev->lib_Flags=LIBF_SUMUSED|LIBF_CHANGED;dev->lib_Version=DEVICE_VERSION;dev->lib_Revision=DEVICE_REVISION;dev->lib_IdString=(APTR)device_id_string;is_open=FALSE;vblank_added=FALSE;dwc2_irq_added=FALSE;unit_process=NULL;unit_task=NULL;unit_parent=NULL;unit_port=NULL;unit_task_ready=FALSE;unit_task_stopping=FALSE;unit_rx_sigbit=unit_abort_sigbit=unit_usb_sigbit=-1;unit_rx_sigmask=unit_abort_sigmask=unit_usb_sigmask=0;hw_ready=FALSE;DOSBase=NULL;return dev;}
static BPTR __attribute__((used)) expunge(struct Library *dev asm("a6")){return do_expunge(dev);}static void __attribute__((used)) open(struct Library *dev asm("a6"),struct IORequest *io asm("a1"),ULONG unit asm("d0"),ULONG flags asm("d1")){do_open(dev,io,unit,flags);}static BPTR __attribute__((used)) close(struct Library *dev asm("a6"),struct IORequest *io asm("a1")){return do_close(dev,io);}static void __attribute__((used)) begin_io(struct Library *dev asm("a6"),struct IORequest *io asm("a1")){do_begin_io(dev,io);}static ULONG __attribute__((used)) abort_io(struct Library *dev asm("a6"),struct IORequest *io asm("a1")){return do_abort_io(dev,io);}static ULONG device_vectors[]={(ULONG)open,(ULONG)close,(ULONG)expunge,0,(ULONG)begin_io,(ULONG)abort_io,(ULONG)-1};const ULONG auto_init_tables[4]={sizeof(struct Library),(ULONG)device_vectors,0,(ULONG)init_device};
