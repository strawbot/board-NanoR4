// usb_ncm.c — IP over USB: TinyUSB CDC-NCM <-> lwIP, DHCP server, HTTP server
//
// The board is 192.168.7.1; the host's NCM interface is offered 192.168.7.2
// by the DHCP server, then browses to http://192.168.7.1/.
//
// Everything is event driven: frames arrive through tud_network_recv_cb()
// (called from tud_task() in usb_action), and lwIP's own timers run from one
// tea timer that re-arms for exactly the time lwIP says it next needs.
// Nothing here blocks.
//
// Only compiled into env:nano_r4_web (-D WITH_USB_NCM); other builds get just
// the `ncm` CLI stub.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "tea.h"
#include "printers.h"
#include "usb_ncm.h"

#ifndef WITH_USB_NCM

void usb_ncm_cli_show(void) {
    print("not built into this firmware -- see env:nano_r4_web"); printCr();
}

#else

#include "hal_data.h"
#include "tusb.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "dhserver.h"

#include "http_server.h"
#include "robot_web.h"
#include "telnet_server.h"

#define INIT_IP4(a, b, c, d)  { PP_HTONL(LWIP_MAKEU32(a, b, c, d)) }
#define LWIP_TICK_MAX_MS      250       // longest we sleep between lwIP timer checks

// ── Addresses ────────────────────────────────────────────────────────────────

// The host's NCM interface MAC. The netif's own MAC is this with the low bit
// flipped, so the two ends differ. Filled from the chip's unique ID in
// usb_ncm_init() -- but the USB stack reads it earlier, via
// usb_ncm_mac_string(), so that function fills it too.
uint8_t tud_network_mac_address[6];

static const ip4_addr_t ipaddr  = INIT_IP4(192, 168, 7, 1);
static const ip4_addr_t netmask = INIT_IP4(255, 255, 255, 0);
static const ip4_addr_t gateway = INIT_IP4(0, 0, 0, 0);

// Leases offered to the host. Needs to be RAM: the server records MACs here.
static dhcp_entry_t dhcp_entries[] = {
    { {0}, INIT_IP4(192, 168, 7, 2), 24 * 60 * 60 },
    { {0}, INIT_IP4(192, 168, 7, 3), 24 * 60 * 60 },
};

// No router and no DNS: the host keeps its internet route on its other
// interfaces, and treats us as a plain link-local network.
static const dhcp_config_t dhcp_config = {
    .router    = INIT_IP4(0, 0, 0, 0),
    .port      = 67,
    .dns       = INIT_IP4(0, 0, 0, 0),
    .domain    = NULL,
    .num_entry = sizeof(dhcp_entries) / sizeof(dhcp_entries[0]),
    .entries   = dhcp_entries,
};

static struct netif ncm_netif;

// ── Counters for the `ncm` word ──────────────────────────────────────────────

static uint32_t rx_frames, rx_nobuf, tx_frames, tx_refused;
static bool     rx_stalled;             // last frame was refused for lack of a pbuf

// ── MAC from the chip's unique ID ────────────────────────────────────────────

static void mac_fill(void) {
    if (tud_network_mac_address[0] != 0) return;
    const bsp_unique_id_t *uid = R_BSP_UniqueIdGet();
    uint32_t h = 2166136261u;                           // FNV-1a over the 128-bit ID
    for (unsigned i = 0; i < sizeof(uid->unique_id_bytes); i++) {
        h = (h ^ uid->unique_id_bytes[i]) * 16777619u;
    }
    tud_network_mac_address[0] = 0x02;                  // locally administered, unicast
    tud_network_mac_address[1] = 0x52;                  // 'R' for robot
    tud_network_mac_address[2] = (uint8_t)(h >> 24);
    tud_network_mac_address[3] = (uint8_t)(h >> 16);
    tud_network_mac_address[4] = (uint8_t)(h >> 8);
    tud_network_mac_address[5] = (uint8_t)(h & 0xFE);   // even: the netif end is +1
}

const char *usb_ncm_mac_string(void) {
    static char s[13];
    mac_fill();
    for (int i = 0; i < 6; i++) {
        s[2 * i]     = "0123456789ABCDEF"[tud_network_mac_address[i] >> 4];
        s[2 * i + 1] = "0123456789ABCDEF"[tud_network_mac_address[i] & 0xF];
    }
    return s;
}

// ── lwIP time ────────────────────────────────────────────────────────────────

uint32_t sys_now(void) { return (uint32_t)getTime(); }

// Run whatever lwIP timers are due, then sleep until the next one is.
static void ncm_service(void) {
    sys_check_timeouts();
    if (rx_stalled) {                                   // a pbuf may be free again
        rx_stalled = false;
        tud_network_recv_renew();
    }
    uint32_t wait = sys_timeouts_sleeptime();
    if (wait > LWIP_TICK_MAX_MS) wait = LWIP_TICK_MAX_MS;
    if (wait < 1) wait = 1;
    in(msec(wait), ncm_service);
}

// ── Frames: lwIP -> USB ──────────────────────────────────────────────────────

// Never waits for the USB side: if both transmit buffers are busy the frame
// is refused and TCP retransmits it.
static err_t ncm_linkoutput(struct netif *netif, struct pbuf *p) {
    (void) netif;
    if (!tud_ready() || !tud_network_can_xmit(p->tot_len)) {
        tx_refused++;
        return ERR_MEM;
    }
    tud_network_xmit(p, 0);
    tx_frames++;
    return ERR_OK;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    (void) arg;
    struct pbuf *p = (struct pbuf *) ref;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

// ── Frames: USB -> lwIP ──────────────────────────────────────────────────────

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (size == 0) return true;

    struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (p == NULL) {
        rx_nobuf++;
        rx_stalled = true;                              // ncm_service retries
        return false;
    }
    pbuf_take(p, src, size);
    rx_frames++;
    if (ncm_netif.input(p, &ncm_netif) != ERR_OK) pbuf_free(p);
    tud_network_recv_renew();                           // done with this frame
    return true;
}

void tud_network_init_cb(void) { }

// ── netif ────────────────────────────────────────────────────────────────────

static err_t ncm_ip4_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *addr) {
    return etharp_output(netif, p, addr);
}

static err_t ncm_netif_init(struct netif *netif) {
    netif->mtu        = CFG_TUD_NET_MTU - 14;
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->name[0]    = 'E';
    netif->name[1]    = 'X';
    netif->linkoutput = ncm_linkoutput;
    netif->output     = ncm_ip4_output;
    return ERR_OK;
}

// Tell the host when the "cable" goes in or out.
static void ncm_link_callback(struct netif *netif) {
    tud_network_link_state(0, netif_is_link_up(netif));
}

// ── Init ─────────────────────────────────────────────────────────────────────

void usb_ncm_init(void) {
    mac_fill();
    lwip_init();

    memcpy(ncm_netif.hwaddr, tud_network_mac_address, 6);
    ncm_netif.hwaddr_len = 6;
    ncm_netif.hwaddr[5] ^= 0x01;

    netif_add(&ncm_netif, &ipaddr, &netmask, &gateway, NULL, ncm_netif_init, ethernet_input);
    netif_set_default(&ncm_netif);
    netif_set_link_callback(&ncm_netif, ncm_link_callback);
    netif_set_up(&ncm_netif);
    netif_set_link_up(&ncm_netif);

    dhserv_init(&dhcp_config);

    http_server_init();
    robot_web_init();
    http_server_set_routes(robot_web_routes, robot_web_route_count);
    http_server_start();
    telnet_server_init();
    telnet_server_start();

    ncm_service();
}

// ── CLI ──────────────────────────────────────────────────────────────────────

void usb_ncm_cli_show(void) {
    uint8_t active, idle;
    http_server_stats(&active, &idle);

    print("ncm mac "); print(usb_ncm_mac_string());
    print("  ip 192.168.7.1  usb "); print(tud_ready() ? "ready" : "not ready");
    print("  link "); print(netif_is_link_up(&ncm_netif) ? "up" : "down"); printCr();
    print("rx "); printDec(rx_frames); print(" (no-buf "); printDec(rx_nobuf);
    print(")  tx "); printDec(tx_frames); print(" (refused "); printDec(tx_refused);
    print(")  http active "); printDec(active); print(" idle "); printDec(idle); printCr();
}

#endif // WITH_USB_NCM
