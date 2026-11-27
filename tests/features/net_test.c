/* net.socket_udp: Adressen, Senden und Empfangen über Loopback (IPv4 und IPv6). */
#include "nhal/features/net/socket_udp.h"
#include "check.h"

#include <string.h>

static const nhal_net_socket_udp_v1* N;

static void roundtrip(const char* bind_text) {
    nhal_net_address bind;
    CHECK_OK(N->parse_address(bind_text, &bind));
    nhal_udp_socket* a = NULL;
    nhal_udp_socket* b = NULL;
    nhal_status s = N->open(&bind, &a);
    if (bind.family == NHAL_NET_IPV6 && !nhal_ok(s)) { /* Rechner ohne IPv6 */
        printf("net_test: %s übersprungen (%s)\n", bind_text, s.reason);
        return;
    }
    CHECK_OK(s);
    CHECK_OK(N->open(&bind, &b));
    if (!a || !b) return;
    nhal_net_address addr_a, addr_b;
    CHECK_OK(N->local_address(a, &addr_a));
    CHECK_OK(N->local_address(b, &addr_b));
    CHECK(addr_a.port != 0 && addr_a.port != addr_b.port);

    char buf[64];
    nhal_udp_datagram d;
    CHECK_OK(N->recv_from(b, buf, sizeof buf, &d));
    CHECK(!d.received); /* nichts da ist kein Fehler */

    CHECK_OK(N->send_to(a, &addr_b, "hallo xernel", 12));
    bool readable = false;
    CHECK_OK(N->wait(b, 2000, &readable));
    CHECK(readable);
    CHECK_OK(N->recv_from(b, buf, sizeof buf, &d));
    CHECK(d.received && !d.truncated && d.size == 12 && memcmp(buf, "hallo xernel", 12) == 0);
    CHECK(d.from.port == addr_a.port && d.from.family == bind.family);

    CHECK_OK(N->send_to(a, &addr_b, "0123456789", 10)); /* zu kleiner Puffer */
    CHECK_OK(N->wait(b, 2000, &readable));
    CHECK_OK(N->recv_from(b, buf, 4, &d));
    CHECK(d.received && d.truncated && d.size == 4 && memcmp(buf, "0123", 4) == 0);

    CHECK_OK(N->wait(b, 0, &readable));
    CHECK(!readable);

    nhal_udp_socket* c = NULL;
    CHECK(N->open(&addr_a, &c).code == NHAL_ERR_BUSY); /* Port belegt */
    N->close(a);
    N->close(b);
}

int main(void) {
    const nhal_request req = { NHAL_FEATURE_NET_SOCKET_UDP, 1u, NHAL_REQUIRED };
    const nhal_manifest m = { &req, 1 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    nhal_context* ctx = NULL;
    CHECK_OK(nhal_init(&info, &ctx));
    N = nhal_get_net_socket_udp_v1(ctx);
    if (!N) return check_report("net_test");

    nhal_net_address a;
    char text[NHAL_NET_ADDRESS_TEXT_MAX];
    CHECK_OK(N->parse_address("198.51.100.20:9000", &a));
    CHECK(a.family == NHAL_NET_IPV4 && a.port == 9000 && a.bytes[0] == 198 && a.bytes[3] == 20);
    N->format_address(&a, text, sizeof text);
    CHECK(strcmp(text, "198.51.100.20:9000") == 0);
    CHECK_OK(N->parse_address("[2001:db8::1]:443", &a));
    CHECK(a.family == NHAL_NET_IPV6 && a.port == 443 && a.bytes[0] == 0x20 && a.bytes[15] == 1);
    N->format_address(&a, text, sizeof text);
    CHECK(strcmp(text, "[2001:db8::1]:443") == 0);
    CHECK(N->parse_address("localhost:80", &a).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(N->parse_address("1.2.3.4:70000", &a).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(N->parse_address("1.2.3.4", &a).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(N->parse_address("[::1]80", &a).code == NHAL_ERR_INVALID_ARGUMENT);

    roundtrip("127.0.0.1:0");
    roundtrip("[::1]:0");
    nhal_shutdown(ctx);
    return check_report("net_test");
}
