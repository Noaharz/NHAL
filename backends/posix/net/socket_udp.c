/*
 * net.socket_udp – POSIX (macOS, Linux): BSD-Sockets, nicht blockierend.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "nhal/features/net/socket_udp.h"
#include "nhal/nhal_backend.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct nhal_udp_socket {
    int fd;
    nhal_net_family family;
};

static const nhal_allocator* g_alloc = NULL;

static nhal_status net_error(int err) {
    switch (err) {
        case EADDRINUSE: return nhal_status_error(NHAL_ERR_BUSY, "Adresse bzw. Port ist schon belegt");
        case EADDRNOTAVAIL: return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Adresse gehört nicht zu diesem Rechner");
        case EACCES:
        case EPERM: return nhal_status_error(NHAL_ERR_IO, "Zugriff verweigert (Port oder Broadcast)");
        case EAGAIN: return nhal_status_error(NHAL_ERR_BUSY, "Sendepuffer ist voll");
        case EMSGSIZE: return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Datagramm ist zu groß");
        case ENETUNREACH:
        case EHOSTUNREACH: return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Ziel ist nicht erreichbar");
        case EAFNOSUPPORT: return nhal_status_error(NHAL_ERR_UNSUPPORTED, "Adressfamilie wird nicht unterstützt");
        case EMFILE:
        case ENFILE: return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Zu viele offene Sockets");
        case ENOBUFS:
        case ENOMEM: return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Socket");
        default: return nhal_status_error(NHAL_ERR_IO, "Netzwerkfehler");
    }
}

static socklen_t to_sockaddr(const nhal_net_address* a, struct sockaddr_storage* ss) {
    memset(ss, 0, sizeof *ss);
    if (a->family == NHAL_NET_IPV4) {
        struct sockaddr_in* in = (struct sockaddr_in*)ss;
        in->sin_family = AF_INET;
        in->sin_port = htons(a->port);
        memcpy(&in->sin_addr, a->bytes, 4);
        return sizeof *in;
    }
    struct sockaddr_in6* in6 = (struct sockaddr_in6*)ss;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(a->port);
    memcpy(&in6->sin6_addr, a->bytes, 16);
    return sizeof *in6;
}

static void from_sockaddr(const struct sockaddr_storage* ss, nhal_net_address* a) {
    memset(a, 0, sizeof *a);
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in* in = (const struct sockaddr_in*)ss;
        a->family = NHAL_NET_IPV4;
        a->port = ntohs(in->sin_port);
        memcpy(a->bytes, &in->sin_addr, 4);
    } else {
        const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)ss;
        a->family = NHAL_NET_IPV6;
        a->port = ntohs(in6->sin6_port);
        memcpy(a->bytes, &in6->sin6_addr, 16);
    }
}

static bool parse_port(const char* s, uint16_t* port) {
    if (!*s) return false;
    unsigned long v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10u + (unsigned long)(*s - '0');
        if (v > 65535u) return false;
    }
    *port = (uint16_t)v;
    return true;
}

static nhal_status udp_parse_address(const char* text, nhal_net_address* out) {
    static const nhal_status bad = { NHAL_ERR_INVALID_ARGUMENT, NHAL_FEATURE_NONE,
                                     "Adresse muss die Form 1.2.3.4:port oder [::1]:port haben" };
    if (!text || !out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "text oder out ist NULL");
    char host[64];
    const char* port;
    memset(out, 0, sizeof *out);
    if (text[0] == '[') {
        const char* end = strchr(text, ']');
        if (!end || end[1] != ':' || (size_t)(end - text - 1) >= sizeof host) return bad;
        memcpy(host, text + 1, (size_t)(end - text - 1));
        host[end - text - 1] = '\0';
        port = end + 2;
        if (inet_pton(AF_INET6, host, out->bytes) != 1) return bad;
        out->family = NHAL_NET_IPV6;
    } else {
        const char* colon = strrchr(text, ':');
        if (!colon || (size_t)(colon - text) >= sizeof host) return bad;
        memcpy(host, text, (size_t)(colon - text));
        host[colon - text] = '\0';
        port = colon + 1;
        if (inet_pton(AF_INET, host, out->bytes) != 1) return bad;
        out->family = NHAL_NET_IPV4;
    }
    if (!parse_port(port, &out->port)) return bad;
    return nhal_status_ok();
}

static size_t udp_format_address(const nhal_net_address* a, char* buf, size_t capacity) {
    char host[INET6_ADDRSTRLEN];
    if (!a || !buf || capacity == 0) return 0;
    const int af = a->family == NHAL_NET_IPV4 ? AF_INET : AF_INET6;
    if (!inet_ntop(af, a->bytes, host, sizeof host)) host[0] = '\0';
    const int n = a->family == NHAL_NET_IPV4 ? snprintf(buf, capacity, "%s:%u", host, (unsigned)a->port)
                                             : snprintf(buf, capacity, "[%s]:%u", host, (unsigned)a->port);
    if (n < 0) return 0;
    return (size_t)n < capacity ? (size_t)n : capacity - 1;
}

static nhal_status udp_open(const nhal_net_address* bind_to, nhal_udp_socket** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    if (!bind_to || (bind_to->family != NHAL_NET_IPV4 && bind_to->family != NHAL_NET_IPV6)) {
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Bind-Adresse fehlt oder hat keine gültige Familie");
    }
    const int af = bind_to->family == NHAL_NET_IPV4 ? AF_INET : AF_INET6;
    const int fd = socket(af, SOCK_DGRAM, 0);
    if (fd < 0) return net_error(errno);
    const int one = 1;
    if (af == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_storage ss;
    const socklen_t len = to_sockaddr(bind_to, &ss);
    if (bind(fd, (const struct sockaddr*)&ss, len) != 0) {
        const nhal_status s = net_error(errno);
        close(fd);
        return s;
    }
    nhal_udp_socket* sock = (nhal_udp_socket*)g_alloc->alloc(g_alloc->user, sizeof *sock, _Alignof(nhal_udp_socket));
    if (!sock) {
        close(fd);
        return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Socket");
    }
    sock->fd = fd;
    sock->family = bind_to->family;
    *out = sock;
    return nhal_status_ok();
}

static void udp_close(nhal_udp_socket* s) {
    if (!s) return;
    close(s->fd);
    g_alloc->free(g_alloc->user, s, sizeof *s);
}

static nhal_status udp_local_address(nhal_udp_socket* s, nhal_net_address* out) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getsockname(s->fd, (struct sockaddr*)&ss, &len) != 0) return net_error(errno);
    from_sockaddr(&ss, out);
    return nhal_status_ok();
}

static nhal_status udp_send_to(nhal_udp_socket* s, const nhal_net_address* to, const void* data, size_t size) {
    if (!to || (size && !data)) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "to oder data ist NULL");
    if (to->family != s->family) {
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Zieladresse hat eine andere Familie als der Socket");
    }
    struct sockaddr_storage ss;
    const socklen_t len = to_sockaddr(to, &ss);
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    ssize_t n;
    do {
        n = sendto(s->fd, data, size, flags, (const struct sockaddr*)&ss, len);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return net_error(errno == EWOULDBLOCK ? EAGAIN : errno);
    if ((size_t)n != size) return nhal_status_error(NHAL_ERR_IO, "Datagramm wurde nur teilweise gesendet");
    return nhal_status_ok();
}

static nhal_status udp_recv_from(nhal_udp_socket* s, void* buf, size_t capacity, nhal_udp_datagram* out) {
    if (!out || (capacity && !buf)) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "buf oder out ist NULL");
    memset(out, 0, sizeof *out);
    struct sockaddr_storage ss;
    struct iovec iov = { buf, capacity };
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_name = &ss;
    msg.msg_namelen = sizeof ss;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    ssize_t n;
    do {
        n = recvmsg(s->fd, &msg, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return nhal_status_ok(); /* nichts da */
        /* ICMP "Port nicht erreichbar" einer früheren Sendung meldet das System
         * hier; das betrifft kein empfangenes Datagramm. */
        if (errno == ECONNREFUSED) return nhal_status_ok();
        return net_error(errno);
    }
    out->received = true;
    out->truncated = (msg.msg_flags & MSG_TRUNC) != 0;
    out->size = (size_t)n;
    from_sockaddr(&ss, &out->from);
    return nhal_status_ok();
}

static nhal_status udp_wait(nhal_udp_socket* s, uint32_t timeout_ms, bool* readable) {
    struct pollfd p = { s->fd, POLLIN, 0 };
    int r;
    do {
        r = poll(&p, 1, timeout_ms > 0x7fffffffu ? 0x7fffffff : (int)timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return net_error(errno);
    if (readable) *readable = r > 0 && (p.revents & (POLLIN | POLLERR)) != 0;
    return nhal_status_ok();
}

static nhal_status feature_init(const nhal_allocator* allocator) {
    g_alloc = allocator;
    return nhal_status_ok();
}

static void feature_shutdown(void) { g_alloc = NULL; }

static const nhal_net_socket_udp_v1 table = {
    sizeof(nhal_net_socket_udp_v1), udp_parse_address, udp_format_address, udp_open, udp_close,
    udp_local_address, udp_send_to, udp_recv_from, udp_wait,
};

const nhal_feature_impl nhal_impl_net_socket_udp = { 1u, feature_init, feature_shutdown, &table };
