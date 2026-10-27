// lwipopts.h — lwIP 2.1.3 configuration for the Nano R4 USB web page.
//
// Sized for an RA4M1 with 32 KB of RAM: one netif (the USB NCM link), a
// handful of HTTP connections, a DHCP server, no DNS/DHCP-client/IPv6.
// Every pool below is deliberately small; raise them only after checking
// `pio run -e nano_r4_web` RAM usage.

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SINGLE_NETIF           1
#define LWIP_NOASSERT               1
// lwIP is only ever called from the tea scheduler (tud_task() runs there, not
// in the USB interrupt), so there is no interrupt-vs-main race to guard.
#define SYS_LIGHTWEIGHT_PROT        0
#define MEM_ALIGNMENT               4

// ── Protocols ────────────────────────────────────────────────────────────────
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1       // ping is the first thing to try
#define LWIP_UDP                    1       // DHCP server
#define LWIP_TCP                    1
#define LWIP_RAW                    0
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_DNS                    0
#define LWIP_IGMP                   0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_STATS                  0
#define LWIP_NETIF_LINK_CALLBACK    1
#define IP_REASSEMBLY               0
#define IP_FRAG                     0
#define ETH_PAD_SIZE                0

// The DHCP server must see client packets before the host has an address.
#define LWIP_IP_ACCEPT_UDP_PORT(p)  ((p) == PP_NTOHS(67))

// ── Link sizing ──────────────────────────────────────────────────────────────
// MTU 576 (the IPv4 minimum every host must accept) keeps every pbuf and NCM
// transfer buffer small. The host's own MTU stays 1500, but our advertised
// TCP MSS of 536 keeps its TCP segments within our limit.
#define TCP_MSS                     536
#define TCP_SND_BUF                 (2 * TCP_MSS)
#define TCP_WND                     (2 * TCP_MSS)

// ── Memory ───────────────────────────────────────────────────────────────────
#define MEM_SIZE                    2048    // tcp_write copies (SSE pushes) come from here
#define MEMP_NUM_PBUF               4
#define PBUF_POOL_SIZE              3       // received frames; each is handled and freed before the next arrives
#define MEMP_NUM_UDP_PCB            2
#define MEMP_NUM_TCP_PCB            6       // 3 HTTP + 2 telnet sessions, +1 spare
#define MEMP_NUM_TCP_PCB_LISTEN     2       // HTTP + telnet
#define MEMP_NUM_TCP_SEG            8
#define MEMP_NUM_SYS_TIMEOUT        8
#define MEMP_NUM_NETBUF             0
#define MEMP_NUM_NETCONN            0

// The USB link cannot corrupt frames, and the host may hand us frames with
// checksum offload pending, so don't verify what arrives; do generate.
#define CHECKSUM_CHECK_IP           0
#define CHECKSUM_CHECK_UDP          0
#define CHECKSUM_CHECK_TCP          0

#endif // LWIPOPTS_H
