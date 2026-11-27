/*
 * net.socket_udp, Version 1: UDP-Sockets.
 *
 * Semantik (gleich auf allen Plattformen):
 *   - Sockets sind nicht blockierend. send_to() und recv_from() kehren immer
 *     sofort zurück. wait() wartet gezielt auf eingehende Daten.
 *   - recv_from() liefert höchstens ein Datagramm. Ist keins da, ist
 *     datagram.received false; das ist kein Fehler. Ist der Puffer zu klein,
 *     wird gekürzt und datagram.truncated gesetzt.
 *   - send_to() schickt ein Datagramm ganz oder meldet einen Fehler. Ist der
 *     Sendepuffer des Systems voll, kommt NHAL_ERR_BUSY.
 *   - Adressen sind numerisch (IPv4 "1.2.3.4:5", IPv6 "[::1]:5"). Namen
 *     auflösen gehört nicht zu diesem Feature.
 *   - IPv6-Sockets sind reine IPv6-Sockets (kein Dual-Stack), überall gleich.
 *   - Ein Socket darf nicht gleichzeitig aus mehreren Threads benutzt werden.
 */
#ifndef NHAL_FEATURES_NET_SOCKET_UDP_H
#define NHAL_FEATURES_NET_SOCKET_UDP_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nhal_net_family {
    NHAL_NET_IPV4 = 4,
    NHAL_NET_IPV6 = 6
} nhal_net_family;

typedef struct nhal_net_address {
    nhal_net_family family;
    uint8_t bytes[16]; /* IPv4: die ersten 4 Bytes, Netzwerk-Reihenfolge */
    uint16_t port;     /* Host-Reihenfolge */
} nhal_net_address;

/* Längster Text von format_address inklusive Nullbyte. */
#define NHAL_NET_ADDRESS_TEXT_MAX 48u

typedef struct nhal_udp_datagram {
    bool received;
    bool truncated;
    size_t size; /* Bytes in buf */
    nhal_net_address from;
} nhal_udp_datagram;

typedef struct nhal_udp_socket nhal_udp_socket;

typedef struct nhal_net_socket_udp_v1 {
    uint32_t struct_size;
    nhal_status (*parse_address)(const char* text, nhal_net_address* out);
    /* Schreibt z. B. "127.0.0.1:9000"; gibt die Länge ohne Nullbyte zurück. */
    size_t (*format_address)(const nhal_net_address* address, char* buf, size_t capacity);
    /* Port 0 = vom System gewählt (local_address() verrät ihn). */
    nhal_status (*open)(const nhal_net_address* bind, nhal_udp_socket** out);
    void (*close)(nhal_udp_socket* socket);
    nhal_status (*local_address)(nhal_udp_socket* socket, nhal_net_address* out);
    nhal_status (*send_to)(nhal_udp_socket* socket, const nhal_net_address* to, const void* data, size_t size);
    nhal_status (*recv_from)(nhal_udp_socket* socket, void* buf, size_t capacity, nhal_udp_datagram* out);
    /* Wartet bis zu timeout_ms (0 = nur nachsehen) auf Daten. */
    nhal_status (*wait)(nhal_udp_socket* socket, uint32_t timeout_ms, bool* readable);
} nhal_net_socket_udp_v1;

static inline const nhal_net_socket_udp_v1* nhal_get_net_socket_udp_v1(const nhal_context* ctx) {
    return (const nhal_net_socket_udp_v1*)nhal_get(ctx, NHAL_FEATURE_NET_SOCKET_UDP, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_NET_SOCKET_UDP_H */
