/* usb_cdc.c — minimal bare-metal USB CDC-ACM serial for the RP2350 (no SDK, no TinyUSB, no interrupts).
 *
 * The whole USB device is driven by raw register writes and POLLED from usb_cdc_task(), so it needs no
 * NVIC/IRQ and no RTOS — call usb_cdc_task() from your main loop. It enumerates as a standard USB serial
 * port (/dev/cu.usbmodem*, /dev/ttyACM*, or a COM port) and also exposes the picotool "reset" vendor
 * interface, so `picotool ... -f` can reboot it to BOOTSEL for flashing without touching the board.
 *
 * The decisive, non-obvious requirement is in usb_cdc_init(): clk_sys must be >= clk_usb (48 MHz). The
 * RP2350 USB SIE reads its DPRAM/registers on clk_sys; the bootrom hands off with clk_sys on the ~8 MHz
 * ROSC, and with clk_sys that slow the controller never even asserts the D+ pullup. Every other reference
 * (pico-sdk / TinyUSB) sidesteps this by running clk_sys fast. See README.md for the full list.
 *
 * Register values are from the pico-sdk RP2350 headers + the RP2350 datasheet. MIT licensed. */
#include "usb_cdc.h"

#define REG(a)  (*(volatile uint32_t *)(a))
#define WAIT(cond) do{ for(volatile uint32_t _t=0; _t<2000000u; _t++){ if(cond) break; } }while(0)  /* never hang */

#define USB_REGS    0x50110000u
#define USB_DPRAM   0x50100000u
#define RESETS_SET  0x40022000u
#define RESETS_CLR  0x40023000u
#define RESETS_DONE 0x40020008u

/* --- USB controller registers --- */
#define ADDR_ENDP    (USB_REGS + 0x00u)
#define MAIN_CTRL    (USB_REGS + 0x40u)
#define SIE_CTRL     (USB_REGS + 0x4Cu)
#define SIE_STATUS   (USB_REGS + 0x50u)
#define BUFF_STATUS  (USB_REGS + 0x58u)
#define USB_MUXING   (USB_REGS + 0x74u)
#define USB_PWR      (USB_REGS + 0x78u)
#define USB_INTE     (USB_REGS + 0x90u)
#define USB_INTS     (USB_REGS + 0x98u)
#define SIE_CTRL_SET (USB_REGS + 0x2000u + 0x4Cu)          /* atomic SET alias */
/* --- DPRAM layout --- */
#define SETUP_PKT    (USB_DPRAM + 0x00u)
#define EP_CTRL(ep,in)  (USB_DPRAM + 0x08u + ((ep)-1u)*8u + ((in)?0u:4u))
#define BUF_CTRL(ep,in) (USB_DPRAM + 0x80u + (ep)*8u + ((in)?0u:4u))
#define EP0_BUF      (USB_DPRAM + 0x100u)
#define EP2_IN_BUF   (USB_DPRAM + 0x180u)                  /* bulk IN  (serial TX to host)   */
#define EP3_OUT_BUF  (USB_DPRAM + 0x1C0u)                  /* bulk OUT (serial RX from host) */

#define BUF_AVAIL   (1u<<10)
#define BUF_FULL    (1u<<15)
#define BUF_LEN     0x3FFu
#define BUF_DATA1   (1u<<13)

struct setup { uint8_t bmRequestType, bRequest; uint16_t wValue, wIndex, wLength; };

/* ---- descriptors ---- VID/PID 0x2e8a/0x000a is Raspberry Pi's; keeping it lets picotool -f drive us. */
static const uint8_t desc_device[18] = {
    18, 1, 0x00,0x02, 0xEF,0x02,0x01, 64,              /* USB2.0, misc/IAD composite, EP0 = 64 */
    0x8A,0x2E, 0x0A,0x00, 0x00,0x01,                   /* idVendor 0x2e8a, idProduct 0x000a */
    1,2,3, 1 };                                         /* iManufacturer, iProduct, iSerial, 1 config */

/* config: IAD + CDC control(0) + CDC data(1) + picotool RESET(2). EPs: 1 IN notify, 2 IN bulk, 3 OUT bulk */
static const uint8_t desc_config[] = {
    9, 2, 84, 0, 3, 1, 0, 0x80, 250,
    8, 11, 0, 2, 0x02,0x02,0x00, 0,                    /* IAD (interfaces 0-1 = CDC) */
    9, 4, 0, 0, 1, 0x02,0x02,0x00, 0,                  /* interface 0: CDC control */
    5, 0x24, 0x00, 0x10,0x01,                          /*   CDC header */
    5, 0x24, 0x01, 0x00, 1,                            /*   call management */
    4, 0x24, 0x02, 0x02,                               /*   ACM */
    5, 0x24, 0x06, 0, 1,                               /*   union: master 0, slave 1 */
    7, 5, 0x81, 0x03, 8,0, 16,                         /*   EP1 IN interrupt (notify, unused) */
    9, 4, 1, 0, 2, 0x0A,0x00,0x00, 0,                  /* interface 1: CDC data */
    7, 5, 0x82, 0x02, 64,0, 0,                         /*   EP2 IN  bulk (TX) */
    7, 5, 0x03, 0x02, 64,0, 0,                         /*   EP3 OUT bulk (RX) */
    9, 4, 2, 0, 0, 0xFF,0x00,0x01, 0,                  /* interface 2: picotool RESET (vendor FF/00/01) */
};
#define CONFIG_TOTAL 84u

static const uint8_t str_lang[4] = { 4, 3, 0x09,0x04 };
static uint8_t g_addr = 0, g_pending_addr = 0;
static uint8_t g_txpid = 0, g_rxpid = 0, g_ep0pid = 1;

static void ascii_str(uint8_t *o, const char *s){ int n=0; while(s[n]) n++; o[0]=(uint8_t)(2+n*2); o[1]=3;
    for(int i=0;i<n;i++){ o[2+i*2]=(uint8_t)s[i]; o[3+i*2]=0; } }

/* RP2350 datasheet 12.7.3.7.1 "Concurrent access": write buffer-control WITHOUT the AVAIL bit, wait >=12
 * cycles for the controller to observe it, THEN set AVAIL. Doing it in one write malforms the transfer. */
static void av(uint32_t addr, uint32_t val){
    REG(addr) = val & ~BUF_AVAIL;
    for(volatile uint32_t i=0;i<8u;i++){ }
    REG(addr) = val;
}

/* multi-packet EP0 IN: the 84-byte config descriptor goes as 64 + 20 (source must be static, not stack) */
static const uint8_t *g_ep0_src; static uint16_t g_ep0_rem; static uint8_t g_ep0_data_in = 0;
static uint8_t g_ep0_out_data = 0;      /* a control WRITE's data stage is outstanding on EP0 OUT */
static void ep0_tx_chunk(void){
    uint16_t n = g_ep0_rem > 64u ? 64u : g_ep0_rem;
    for(uint16_t i=0;i<n;i++) ((volatile uint8_t*)EP0_BUF)[i]=g_ep0_src[i];
    av(BUF_CTRL(0,1), (uint32_t)n | BUF_AVAIL | BUF_FULL | (g_ep0pid?BUF_DATA1:0));
    g_ep0pid^=1u; g_ep0_src+=n; g_ep0_rem-=n;
}
static uint8_t g_strbuf[64];
static void ep0_send(const uint8_t *d, uint16_t n){ g_ep0_src=d; g_ep0_rem=n; g_ep0_data_in=1; ep0_tx_chunk(); }
static void ep0_status_in(void){ g_ep0pid=1; g_ep0_rem=0; g_ep0_data_in=0; av(BUF_CTRL(0,1), BUF_AVAIL|BUF_FULL|BUF_DATA1); }

/* ACCEPT THE DATA STAGE OF A CONTROL WRITE.
 *
 * A host->device request with wLength > 0 is three stages: SETUP, an OUT DATA packet, then a
 * zero-length IN status. This driver only ever implemented the first and last. EP0 OUT was armed
 * in exactly one place — to send the status ZLP after an IN data stage — and NEVER to RECEIVE.
 *
 * So SET_LINE_CODING (0x21, 0x20, wLength 7) went: device ACKs the SETUP and immediately arms the
 * IN status; the host then sends seven bytes of OUT data to an unarmed endpoint and is NAKed, and
 * retries until it gives up. That request is one of the ones a host issues WHEN A PROCESS OPENS
 * THE PORT, which is exactly where a fixed cost was measured:
 *
 *     five opens, four different histories (cold / after a clean close / after 10 s of unread
 *     output / after a drained session): 53.26, 53.43, 52.63, 52.96, 52.79 s. Spread 0.8 s.
 *
 * A constant that no history changes is a host waiting out a timer. Enumeration was never affected
 * because enumeration issues no control write with a data stage — which is why the device appeared
 * instantly and then cost a minute to open.
 *
 * The data is received and DISCARDED, deliberately. This is a native CDC device with no UART
 * behind it: there is no hardware that could honour a baud rate or a parity setting, so storing
 * them would only let the device lie back more convincingly. What the host needs is the handshake
 * completed, and that is what this does. */
static void ep0_receive_and_ack(void){ g_ep0pid=0; g_ep0_rem=0; g_ep0_data_in=0; g_ep0_out_data=1;
    /* BUF_DATA1 IS NOT OPTIONAL HERE. The FIRST packet of a control transfer's data stage is
     * always DATA1 (USB 2.0 §8.5.3) — the SETUP packet is DATA0, and the stage that follows
     * toggles. Arming this buffer for DATA0 makes the controller reject the host's packet on a
     * PID mismatch, which is indistinguishable from never arming it at all: the host is NAKed,
     * retries, and times out identically. The first version of this function omitted the bit and
     * changed the measured open latency by nothing, which is what sent me looking elsewhere. */
                                       av(BUF_CTRL(0,0), 64u | BUF_AVAIL | BUF_DATA1); }

void usb_cdc_reboot_bootsel(void){                     /* reboot into the ROM USB bootloader (BOOTSEL) */
    typedef void *(*lookup_t)(uint32_t code, uint32_t flags);
    _Pragma("GCC diagnostic push")
    _Pragma("GCC diagnostic ignored \"-Warray-bounds\"")   /* 0x16 is the RP2350 ROM table-lookup pointer */
    lookup_t lookup = (lookup_t)(uintptr_t)(uint32_t)(*(volatile uint16_t*)0x00000016u);
    _Pragma("GCC diagnostic pop")
    typedef int (*reboot_t)(uint32_t flags, uint32_t delay_ms, uint32_t p0, uint32_t p1);
    reboot_t rom_reboot = (reboot_t)lookup(0x4252u /*'R','B'*/, 0x0004u /*RT_FLAG_FUNC_ARM_SEC*/);
    if(rom_reboot) rom_reboot(0x0002u /*BOOTSEL*/ | 0x0100u /*NO_RETURN*/, 10u, 0u, 0u);
    for(;;){}
}

static void handle_setup(void){
    volatile struct setup *s = (volatile struct setup *)SETUP_PKT;
    g_ep0pid = 1;
    /* A control WRITE that carries data must have its data stage accepted before the status is
     * sent. Checked here, once, for every host->device request rather than per-request, so a
     * class request added later cannot reintroduce the stall by forgetting it. */
    if(!(s->bmRequestType & 0x80) && s->wLength > 0u){ ep0_receive_and_ack(); return; }

    if(s->bmRequestType == 0x00){                                  /* host->device standard */
        if(s->bRequest == 5) g_pending_addr = (uint8_t)(s->wValue & 0x7F);  /* SET_ADDRESS (applied post-status) */
        ep0_status_in();
    } else if(s->bmRequestType == 0x80 && s->bRequest == 6){        /* GET_DESCRIPTOR */
        uint8_t t = (uint8_t)(s->wValue >> 8), idx = (uint8_t)s->wValue;
        uint16_t wl = s->wLength;
        if(t==1) ep0_send(desc_device, wl<18u?wl:18u);
        else if(t==2) ep0_send(desc_config, wl<CONFIG_TOTAL?wl:CONFIG_TOTAL);
        else if(t==3){ if(idx==0) ep0_send(str_lang, wl<4u?wl:4u);
            else { ascii_str(g_strbuf, idx==1?"crowdwave":idx==2?"rp2350-usb-cdc":"0001");
                   ep0_send(g_strbuf, wl<g_strbuf[0]?wl:g_strbuf[0]); } }
        else ep0_status_in();
    } else if((s->bmRequestType & 0x60) == 0x20){                  /* CDC class */
        /* GET_LINE_CODING MUST RETURN A LEGAL CODING, AND THIS USED TO RETURN SEVEN ZEROS:
         * dwDTERate = 0 baud and bDataBits = 0, which describes no serial port that can exist.
         *
         * The host reads the line coding when a process OPENS the port, which is precisely where
         * this board spends a fixed ~53 s — measured five times across four different histories
         * (cold, after a clean close, after unread output, after a drained session) with a spread
         * of 0.8 s. That flatness is the tell: a constant cost that no history changes is a host
         * waiting out a timer, not a backlog and not an unclean release.
         *
         * The layout is CDC PSTN 1.20 §6.3.11: dwDTERate little-endian (4), bCharFormat (1,
         * 0 = 1 stop bit), bParityType (1, 0 = none), bDataBits (1). 115200-8-N-1.
         *
         * The value is FIXED rather than echoed back from SET_LINE_CODING, and that is honest
         * rather than lazy: this is a native CDC device with no UART behind it, so the baud rate
         * is meaningless to the hardware — nothing downstream can honour a rate the host asks
         * for. What matters is that the answer is well-formed. Echoing the host's own request
         * would look more correct and would tell it exactly the same lie. */
        static const uint8_t line_coding[7] = {
            0x00, 0xC2, 0x01, 0x00,   /* dwDTERate = 115200 = 0x0001C200, little-endian */
            0x00,                     /* bCharFormat  = 1 stop bit  */
            0x00,                     /* bParityType  = none        */
            0x08                      /* bDataBits    = 8           */
        };
        if(s->bmRequestType & 0x80) ep0_send(line_coding, sizeof line_coding); else ep0_status_in();
    } else if(s->bmRequestType == 0x41 && s->bRequest == 0x01){    /* picotool RESET_BOOTSEL */
        ep0_status_in(); usb_cdc_reboot_bootsel();
    } else ep0_status_in();
}

/* ---- TX ring (bulk EP2 IN) ---- */
/* THE TX RING DROPS WHEN FULL, AND THAT USED TO BE INVISIBLE.
 *
 * 2048 bytes was not enough once a board had a request/response protocol sharing the link with its
 * diagnostics. With background output flowing, a reply's leading marker could be discarded before it
 * ever reached the host — so the host saw the log pouring in and simultaneously reported "board did
 * not answer", which reads as two unrelated faults and is one.
 *
 * Two changes: the ring is 8 KB, and the drops are COUNTED. A transport that discards data without
 * saying so turns a capacity problem into a protocol mystery — the same defect as a truncating
 * command buffer, at the other end of the same wire. g_tx_dropped is printed by the heartbeat. */
#define TXRING 8192u
static char g_tx[TXRING]; static volatile uint16_t g_th=0, g_tt=0;
volatile uint32_t g_tx_dropped = 0u;
void usb_cdc_putc(char c){ uint16_t nh=(uint16_t)((g_th+1)&(TXRING-1u));
                           if(nh!=g_tt){ g_tx[g_th]=c; g_th=nh; } else g_tx_dropped++; }
void usb_cdc_puts(const char *s){ while(*s) usb_cdc_putc(*s++); }
void usb_cdc_write(const void *data, uint32_t len){ const uint8_t *p=data; while(len--) usb_cdc_putc((char)*p++); }
void usb_cdc_hex(uint32_t v){ usb_cdc_puts("0x"); for(int i=28;i>=0;i-=4){ uint32_t n=(v>>i)&0xF; usb_cdc_putc(n<10?('0'+n):('A'+n-10)); } }

static void tx_pump(void){
    if((REG(BUF_CTRL(2,1)) & BUF_AVAIL) || g_tt==g_th) return;     /* send only when the buffer is free + data queued */
    uint16_t n=0; while(g_tt!=g_th && n<64){ ((volatile uint8_t*)EP2_IN_BUF)[n++]=(uint8_t)g_tx[g_tt]; g_tt=(uint16_t)((g_tt+1)&(TXRING-1u)); }
    av(BUF_CTRL(2,1), (uint32_t)n | BUF_AVAIL | BUF_FULL | (g_txpid?BUF_DATA1:0)); g_txpid^=1u;
}

/* Default RX handler: a tiny "read <hexaddr> <declen>\n" command that dumps memory/registers over the
 * serial link — handy for poking at hardware while debugging. Override usb_cdc_on_rx() to replace it. */
static char g_cmd[64]; static uint8_t g_cn=0;
__attribute__((weak)) void usb_cdc_on_rx(char c){
    if(c=='\n' || c=='\r'){
        g_cmd[g_cn]=0;
        if(g_cmd[0]=='r'){
            uint32_t a=0,len=0; const char *p=g_cmd+4;
            while(*p==' ') p++;
            if(p[0]=='0' && (p[1]|32)=='x') p+=2;               /* skip optional 0x prefix */
            while((*p>='0'&&*p<='9')||((*p|32)>='a'&&(*p|32)<='f')){ char h=(char)(*p|32); a=(a<<4)|(uint32_t)((h<='9')?h-'0':h-'a'+10); p++; }
            while(*p==' ') p++;
            while(*p>='0'&&*p<='9'){ len=len*10+(uint32_t)(*p-'0'); p++; }
            if(len>64u) len=64u;
            usb_cdc_hex(a); usb_cdc_putc(':');
            for(uint32_t i=0;i<len;i+=4){ usb_cdc_putc(' '); usb_cdc_hex(REG(a+i)); }
            usb_cdc_puts("\r\n");
        }
        g_cn=0;
    } else if(g_cn<63) g_cmd[g_cn++]=c;
}

static void rx_pump(void){
    uint32_t bc = REG(BUF_CTRL(3,0));
    if(bc & BUF_FULL){
        uint16_t n = bc & BUF_LEN;
        for(uint16_t i=0;i<n;i++) usb_cdc_on_rx((char)((volatile uint8_t*)EP3_OUT_BUF)[i]);
        av(BUF_CTRL(3,0), 64u | BUF_AVAIL | (g_rxpid?BUF_DATA1:0)); g_rxpid^=1u;   /* re-arm OUT */
    }
}

static void ep_setup(uint32_t ep, uint32_t in, uint32_t type, uint32_t dpram_buf){
    REG(EP_CTRL(ep,in)) = (1u<<31) | (1u<<29) | (type<<26) | (dpram_buf - USB_DPRAM);
}

/* Clocks: PLL_USB VCO 1440 (refdiv 1, fbdiv 120), postdiv 5x2 -> 144 MHz. clk_usb = 144/3 = 48 MHz, and
 * clk_sys = 144/3 = 48 MHz too. The clk_sys line is the crux — see the file header. */
static void clock_init(void){
    REG(0x40048000u)  = 0xAA0u;                        /* XOSC FREQ_RANGE 1-15 MHz */
    REG(0x40048000u) |= (0xFABu << 12);                /* XOSC ENABLE */
    WAIT(REG(0x40048004u) & 0x80000000u);              /* XOSC STABLE */

    REG(RESETS_SET) = 0x00008000u;                     /* reset PLL_USB (clear whatever the bootrom set) */
    REG(RESETS_CLR) = 0x00008000u;
    WAIT(REG(RESETS_DONE) & 0x00008000u);
    REG(0x40058000u) = 1u;                             /* PLL_USB CS refdiv = 1 */
    REG(0x40058008u) = 120u;                           /* FBDIV_INT -> VCO = 12*120 = 1440 MHz */
    REG(0x40058004u) = 0u;                             /* PWR = 0 (power up VCO + postdiv + main) */
    WAIT(REG(0x40058000u) & 0x80000000u);              /* LOCK */
    REG(0x4005800Cu) = (5u << 16) | (2u << 12);        /* PRIM postdiv1=5, postdiv2=2 -> 144 MHz */

    REG(0x40010060u) = (0u << 5) | (1u << 11);         /* CLK_USB_CTRL: AUXSRC=PLL_USB, ENABLE */
    WAIT(REG(0x40010068u) & 0x1u);                     /* CLK_USB_SELECTED */
    REG(0x40010064u) = (3u << 16);                     /* CLK_USB_DIV int=3 -> 48 MHz */

    REG(0x40010040u) = (3u << 16);                     /* CLK_SYS_DIV int=3 */
    REG(0x4001003Cu) = (1u << 5) | 1u;                 /* CLK_SYS_CTRL: AUXSRC=PLL_USB, SRC=aux -> 48 MHz */
    WAIT(REG(0x40010044u) & 0x2u);                     /* CLK_SYS_SELECTED = aux (REQUIRED: >= clk_usb) */
}

void usb_cdc_init(void){
    clock_init();
    REG(RESETS_SET) = 0x10000000u;                     /* reset USBCTRL (clear the bootrom's USB state) */
    for(volatile uint32_t i=0;i<1000u;i++){ }
    REG(RESETS_CLR) = 0x10000000u;
    WAIT(REG(RESETS_DONE) & 0x10000000u);
    for(uint32_t a=USB_DPRAM; a<USB_DPRAM+0x180u; a+=4u) REG(a)=0;   /* clear DPRAM control area */
    REG(USB_MUXING) = 1u | 8u;                          /* TO_PHY | SOFTCON */
    REG(USB_PWR)    = 4u | 8u;                          /* VBUS_DETECT | OVERRIDE_EN */
    ep_setup(1,1,3, EP2_IN_BUF - 0x100u);              /* EP1 IN interrupt (notify) — buffer unused */
    ep_setup(2,1,2, EP2_IN_BUF);                       /* EP2 IN  bulk (TX) */
    ep_setup(3,0,2, EP3_OUT_BUF);                      /* EP3 OUT bulk (RX) */
    av(BUF_CTRL(3,0), 64u | BUF_AVAIL);                /* arm the first OUT */
    REG(MAIN_CTRL) = 1u;                               /* CONTROLLER_EN (this also clears PHY_ISO) */
    REG(SIE_CTRL)  = (1u<<29);                          /* EP0_INT_1BUF (do NOT set RPU_OPT) */
    REG(USB_INTE)  = 0x10u | 0x1000u | 0x10000u;        /* BUFF_STATUS | BUS_RESET | SETUP_REQ (INTS is masked by INTE) */
    REG(SIE_CTRL_SET) = (1u<<16);                      /* PULLUP_EN -> present to host */
}

/* Poll the controller. Call this often from your main loop (no interrupts are used). */
void usb_cdc_task(void){
    uint32_t st = REG(USB_INTS);
    if(st & 0x00001000u){ REG(SIE_STATUS) = 0x00080000u; g_addr=0; REG(ADDR_ENDP)=0; }  /* BUS_RESET (INTS bit12) */
    if(st & 0x00010000u){ REG(SIE_STATUS) = 0x00020000u; handle_setup(); }               /* SETUP_REQ (INTS bit16) */
    uint32_t bs = REG(BUFF_STATUS);
    if(bs){
        REG(BUFF_STATUS) = bs;
        if(bs & 0x2u && g_ep0_out_data){               /* EP0 OUT: a control write's data arrived */
            g_ep0_out_data = 0;
            ep0_status_in();                           /* now, and only now, the status stage */
        }
        if(bs & 0x1u){                                 /* EP0 IN buffer done */
            if(g_ep0_rem > 0u) ep0_tx_chunk();         /* continue a multi-packet descriptor */
            else if(g_ep0_data_in){ g_ep0_data_in=0; g_ep0pid=1; av(BUF_CTRL(0,0), BUF_AVAIL|BUF_DATA1); } /* arm status ZLP */
            else if(g_pending_addr){ g_addr=g_pending_addr; g_pending_addr=0; REG(ADDR_ENDP)=g_addr; }
        }
    }
    tx_pump();
    rx_pump();
}
