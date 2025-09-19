#ifndef __COMMON_H
#define __COMMON_H

#include <stddef.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdatomic.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#define CPU_FREQ_MHZ    60
#define US_PER_TICK     ((double)1 / CPU_FREQ_MHZ)

#define reg_read64(base, reg) (((volatile uint64_t *)(base))[(reg)/8])
#define reg_write64(base, reg, val) (((volatile uint64_t *)(base))[(reg)/8]) = val

#define reg_read32(base, reg) (((volatile uint32_t *)(base))[(reg)/4])
#define reg_write32(base, reg, val) (((volatile uint32_t *)(base))[(reg)/4]) = val

#define reg_read16(base, reg) (((volatile uint16_t *)(base))[(reg)/2])
#define reg_write16(base, reg, val) (((volatile uint16_t *)(base))[(reg)/2]) = val

#define reg_read8(base, reg) (((volatile uint8_t *)(base))[(reg)/1])
#define reg_write8(base, reg, val) (((volatile uint8_t *)(base))[(reg)/1]) = val

/* Optional: ordering helpers for userspace when pairing RAM writes with MMIO */
static inline void mmio_wmb(void) { atomic_thread_fence(memory_order_release); }
static inline void mmio_rmb(void) { atomic_thread_fence(memory_order_acquire); }

#define MAP_INDEX(idx) (((uint64_t) idx) << 40)

struct connection_info {
    uint8_t protocol;

    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;

    uint64_t src_mac;
    uint64_t dst_mac;
};

// Convert string -> binary address.
// family = AF_INET or AF_INET6.
// out must point to struct in_addr (AF_INET) or struct in6_addr (AF_INET6).
// Returns 0 on success, -1 on error.
static inline int string_to_ip(const char *s, int family, void *out) {
    int rc = inet_pton(family, s, out);
    return (rc == 1) ? 0 : -1;
}

// Convert binary address -> string.
// addr points to struct in_addr (AF_INET) or struct in6_addr (AF_INET6).
// buf should be at least INET_ADDRSTRLEN or INET6_ADDRSTRLEN.
// Returns 0 on success, -1 on error.
static inline int ip_to_string(int family, const void *addr, char *buf, size_t buflen) {
    const char *p = inet_ntop(family, addr, buf, (socklen_t)buflen);
    return p ? 0 : -1;
}

/* Convenience wrappers for IPv4 */
static inline int str_to_ipv4(const char *s, uint32_t *addr_be) {
    struct in_addr a;
    if (string_to_ip(s, AF_INET, &a) < 0) return -1;
    *addr_be = a.s_addr;              // network byte order
    return 0;
}
static inline int ipv4_to_str(uint32_t addr_be, char *buf, size_t buflen) {
    struct in_addr a = { .s_addr = addr_be };
    return ip_to_string(AF_INET, &a, buf, buflen);
}

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

/**
 * Parse a MAC address string like "AA:bb:CC:dd:EE:ff" into uint64_t.
 * Stored in the low 48 bits (network order: first byte is MSB).
 * Returns 0 on success, -1 on error.
 */
static inline int mac_str_to_uint64(const char *mac_str, uint64_t *mac_out) {
    unsigned int bytes[6];

    // %x accepts both lowercase and uppercase hex digits
    if (sscanf(mac_str, "%2x:%2x:%2x:%2x:%2x:%2x",
               &bytes[0], &bytes[1], &bytes[2],
               &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return -1; // parse error
    }

    uint64_t mac = 0;
    for (int i = 0; i < 6; i++) {
        mac = (mac << 8) | (bytes[i] & 0xFF);
    }

    *mac_out = mac;
    return 0;
}

/**
 * Convert uint64_t MAC (low 48 bits) back to string "aa:bb:cc:dd:ee:ff".
 * buf must be at least 18 bytes long.
 */
static inline void mac_uint64_to_str(uint64_t mac, char *buf, size_t buflen) {
    if (buflen < 18) return;

    snprintf(buf, buflen,
             "%02" PRIx64 ":%02" PRIx64 ":%02" PRIx64
             ":%02" PRIx64 ":%02" PRIx64 ":%02" PRIx64,
             (mac >> 40) & 0xFF,
             (mac >> 32) & 0xFF,
             (mac >> 24) & 0xFF,
             (mac >> 16) & 0xFF,
             (mac >> 8)  & 0xFF,
             mac & 0xFF);
}

static inline int conn_from_strings(struct connection_info *c, uint8_t protocol,
                                    const char *src_ip_str, uint16_t src_port,
                                    const char *dst_ip_str, uint16_t dst_port)
{
    if (!c || !src_ip_str || !dst_ip_str) { errno = EINVAL; return -1; }

    struct in_addr sip, dip;
    if (inet_pton(AF_INET, src_ip_str, &sip) != 1) { errno = EINVAL; return -1; }
    if (inet_pton(AF_INET, dst_ip_str, &dip) != 1) { errno = EINVAL; return -1; }

    c->protocol = protocol;
    c->src_ip   = ntohl(sip.s_addr);   // host order
    c->dst_ip   = ntohl(dip.s_addr);   // host order
    c->src_port = src_port;            // keep host order; htons() at use site
    c->dst_port = dst_port;
    return 0;
}

static inline int conn_from_strings_mac(struct connection_info *c, uint8_t protocol,
                                    const char *src_mac_str, const char *src_ip_str, uint16_t src_port,
                                    const char *dst_mac_str, const char *dst_ip_str, uint16_t dst_port)
{
    if (!c || !src_ip_str || !dst_ip_str || !src_mac_str || !dst_mac_str) {
        errno = EINVAL; 
        return -1; 
    }

    struct in_addr sip, dip;
    uint64_t src_mac, dst_mac;
    if (inet_pton(AF_INET, src_ip_str, &sip) != 1)      { errno = EINVAL; return -1; }
    if (inet_pton(AF_INET, dst_ip_str, &dip) != 1)      { errno = EINVAL; return -1; }
    if (mac_str_to_uint64(src_mac_str, &src_mac) != 0)  { errno = EINVAL; return -1; }
    if (mac_str_to_uint64(dst_mac_str, &dst_mac) != 0)  { errno = EINVAL; return -1; }

    c->protocol = protocol;
    c->src_ip   = ntohl(sip.s_addr);   // host order
    c->dst_ip   = ntohl(dip.s_addr);   // host order
    c->src_port = src_port;            // keep host order; htons() at use site
    c->dst_port = dst_port;
    c->src_mac = src_mac;
    c->dst_mac = dst_mac;
    return 0;
}


#endif  // __COMMON_H