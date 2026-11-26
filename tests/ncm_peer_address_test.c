#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

void iap2_log(const char *format, ...) { (void)format; }

#include "../src/ncm_bridge.c"

int main(void)
{
    uint8_t frame[14 + 40] = {0};
    struct in6_addr expected;
    char text[INET6_ADDRSTRLEN];

    frame[0] = 0x33; frame[1] = 0x33; frame[2] = 0; frame[3] = 0; frame[4] = 0; frame[5] = 2;
    frame[6] = 0x02; frame[7] = 0x11; frame[8] = 0x22; frame[9] = 0x33; frame[10] = 0x44; frame[11] = 0x55;
    frame[12] = 0x86; frame[13] = 0xdd;
    frame[14] = 0x60;
    frame[20] = 58; /* ICMPv6 neighbor discovery */
    assert(inet_pton(AF_INET6, "fe80::89a3:1317:8514:3551", frame + 14 + 8) == 1);
    assert(inet_pton(AF_INET6, "ff02::2", frame + 14 + 24) == 1);

    g_peer_ipv6_valid = 0;
    memset(text, 0, sizeof(text));
    assert(copy_learned_peer_ipv6(text, sizeof(text)) == 0);
    learn_peer_ipv6(frame + 14, 40);
    assert(g_peer_ipv6_valid == 1);
    assert(inet_pton(AF_INET6, "fe80::89a3:1317:8514:3551", &expected) == 1);
    assert(memcmp(&g_peer_ipv6, &expected, sizeof(expected)) == 0);
    assert(inet_ntop(AF_INET6, &g_peer_ipv6, text, sizeof(text)) != NULL);
    assert(strcmp(text, "fe80::89a3:1317:8514:3551") == 0);

    memset(text, 0, sizeof(text));
    assert(copy_learned_peer_ipv6(text, sizeof(text)) == 1);
    assert(strcmp(text, "fe80::89a3:1317:8514:3551") == 0);

    /* A caller that cannot take the address must see a plain failure. */
    assert(copy_learned_peer_ipv6(NULL, sizeof(text)) == 0);
    memset(text, 0, sizeof(text));
    assert(copy_learned_peer_ipv6(text, 4) == 0);

    /* MAC learning must judge the source address, not the destination: the
     * head unit's first frames are router solicitations to 33:33:00:00:00:02
     * and MLD reports to 33:33:00:00:00:16. Rejecting those left the peer MAC
     * unknown, so the AirPlay SYN went to our own MAC and was dropped. */
    {
        uint8_t rs[14 + 40] = {0};

        rs[0] = 0x33; rs[1] = 0x33; rs[2] = 0; rs[3] = 0; rs[4] = 0; rs[5] = 0x02;
        rs[6] = 0x02; rs[7] = 0x11; rs[8] = 0x22; rs[9] = 0x33; rs[10] = 0x44; rs[11] = 0x55;
        rs[12] = 0x86; rs[13] = 0xdd;

        g_peer_mac_valid = 0;
        learn_peer_mac(rs, sizeof(rs));
        assert(g_peer_mac_valid == 1);
        assert(g_peer_mac[0] == 0x02 && g_peer_mac[5] == 0x55);

        /* A multicast source is not a usable peer address. */
        g_peer_mac_valid = 0;
        rs[6] = 0x03;
        learn_peer_mac(rs, sizeof(rs));
        assert(g_peer_mac_valid == 0);

        /* Frames we sent ourselves must never become the peer address. */
        memcpy(rs + 6, ncm_source_mac, 6);
        learn_peer_mac(rs, sizeof(rs));
        assert(g_peer_mac_valid == 0);

        /* The learned address follows a re-enumerated head unit. */
        rs[6] = 0x0e; rs[7] = 0xd2; rs[8] = 0xef;
        rs[9] = 0xc4; rs[10] = 0xad; rs[11] = 0xde;
        learn_peer_mac(rs, sizeof(rs));
        assert(g_peer_mac_valid == 1);
        assert(g_peer_mac[0] == 0x0e && g_peer_mac[5] == 0xde);

        /* With no learned peer the frame must not be addressed to ourselves. */
        g_peer_mac_valid = 0;
        {
            uint8_t packet[40] = {0};
            uint8_t frame[64];
            size_t frame_len;

            packet[0] = 0x60;
            frame_len = make_ethernet(frame, sizeof(frame), packet, sizeof(packet));
            assert(frame_len == 14 + sizeof(packet));
            assert((frame[0] & 1u) == 1u);              /* broadcast */
            assert(memcmp(frame, ncm_source_mac, 6) != 0);
            assert(memcmp(frame + 6, ncm_source_mac, 6) == 0);
        }
    }

    /* 02:00:00:00:00:02 is the NCM default host address. A head unit that has
     * not configured its NCM interface yet sources frames from it, and sending
     * unicast to that value makes the real NIC drop everything. */
    {
        uint8_t rs[14 + 40] = {0};

        rs[0] = 0x33; rs[1] = 0x33; rs[2] = 0; rs[3] = 0; rs[4] = 0; rs[5] = 0x02;
        rs[6] = 0x02; rs[7] = 0; rs[8] = 0; rs[9] = 0; rs[10] = 0; rs[11] = 0x02;
        rs[12] = 0x86; rs[13] = 0xdd;

        /* It may stand in while nothing better is known... */
        g_peer_mac_valid = 0;
        learn_peer_mac(rs, sizeof(rs));
        assert(g_peer_mac_valid == 1);

        /* ...but it must never displace a real address. */
        assert(ncm_bridge_set_peer_mac("0e:d2:ef:c4:ad:de") == 0);
        assert(g_peer_mac[0] == 0x0e && g_peer_mac[5] == 0xde);
        learn_peer_mac(rs, sizeof(rs));
        assert(g_peer_mac[0] == 0x0e && g_peer_mac[5] == 0xde);

        /* deviceIdentifier wins even over a previously learned real address. */
        assert(ncm_bridge_set_peer_mac("a4:83:e7:11:22:33") == 0);
        assert(g_peer_mac[0] == 0xa4 && g_peer_mac[5] == 0x33);

        /* Rejects anything that cannot be a peer unicast address. */
        assert(ncm_bridge_set_peer_mac("02:00:00:00:00:02") < 0);
        assert(ncm_bridge_set_peer_mac("01:00:5e:00:00:01") < 0);   /* multicast */
        assert(ncm_bridge_set_peer_mac("") < 0);
        assert(ncm_bridge_set_peer_mac(NULL) < 0);
        assert(ncm_bridge_set_peer_mac("zz:00:00:00:00:00") < 0);
        assert(ncm_bridge_set_peer_mac("0e:d2:ef:c4:ad") < 0);
        assert(ncm_bridge_set_peer_mac("0e:d2:ef:c4:ad:de:ff") < 0);
        assert(g_peer_mac[0] == 0xa4 && g_peer_mac[5] == 0x33);
    }
    return 0;
}
