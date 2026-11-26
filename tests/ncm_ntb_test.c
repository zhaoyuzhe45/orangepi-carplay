/* Byte-level conformance test for the NTB we hand to the USB IN endpoint.
 *
 * The head unit parses this with a CDC-NCM host driver, so each NDP entry has
 * to be exactly { le16 wDatagramIndex; le16 wDatagramLength; } repeated every
 * four bytes until a {0,0} terminator. Widening the index to 32 bits shifts
 * the length into the next slot, leaves entry[0].wDatagramLength at zero, and
 * a compliant driver then stops before the first datagram. */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/ncm_bridge.c"

void iap2_log(const char *format, ...) { (void)format; }

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void check_ntb(size_t frame_len, uint16_t sequence)
{
    uint8_t frame[NCM_MAX_FRAME];
    uint8_t block[NCM_MAX_FRAME];
    size_t block_len = 0;
    size_t i;
    const uint8_t *ndp;
    uint32_t datagram_index;
    uint16_t datagram_length;

    for (i = 0; i < frame_len; i++)
        frame[i] = (uint8_t)(i * 7u + 3u);
    /* Plausible ethernet header so the frame looks like what we really send. */
    frame[0] = 0x0e; frame[1] = 0xd2; frame[2] = 0xef;
    frame[3] = 0xc4; frame[4] = 0xad; frame[5] = 0xde;
    frame[6] = 0x72; frame[7] = 0xf5; frame[8] = 0xc0;
    frame[9] = 0x9f; frame[10] = 0x8b; frame[11] = 0xe1;
    frame[12] = 0x86; frame[13] = 0xdd;

    assert(build_ntb(block, sizeof(block), frame, frame_len, sequence,
                     &block_len) == 0);

    /* NTH16 */
    assert(rd32(block) == NCM_NTH16_SIG);
    assert(rd16(block + 4) == NCM_NTH16_BYTES);
    assert(rd16(block + 6) == sequence);
    assert(rd16(block + 8) == block_len);
    assert(rd16(block + 10) == NCM_NTH16_BYTES);
    assert(block_len == NCM_DATAGRAM_OFF + frame_len);

    /* NDP16 header */
    ndp = block + rd16(block + 10);
    assert(rd32(ndp) == NCM_NDP16_SIG);
    assert(rd16(ndp + 4) == NCM_NDP16_BYTES);
    assert(rd16(ndp + 6) == 0);

    /* NDP16 entry[0]: index and length are adjacent 16-bit fields. */
    datagram_index = rd16(ndp + 8);
    datagram_length = rd16(ndp + 10);
    assert(datagram_index == NCM_DATAGRAM_OFF);
    assert(datagram_length == frame_len);
    /* Terminator entry {0,0} right after entry[0]. */
    assert(rd16(ndp + 12) == 0);
    assert(rd16(ndp + 14) == 0);
    /* The NDP must exactly cover header + one entry + terminator. */
    assert(rd16(ndp + 4) == 16);

    /* A host driver stops on length == 0, so this must never be zero. */
    assert(datagram_length != 0);
    /* And the datagram has to be inside the block it claims to live in. */
    assert((size_t)datagram_index + datagram_length <= block_len);

    /* Payload must be byte-identical to what we were asked to send. */
    assert(memcmp(block + datagram_index, frame, frame_len) == 0);
}

int main(void)
{
    /* Small control frames, a full-MTU TCP segment and a max-sized frame. */
    check_ntb(60, 0);
    check_ntb(72, 1);
    check_ntb(1510, 3500);
    check_ntb(1514, 65535);
    check_ntb(NCM_MAX_FRAME - NCM_DATAGRAM_OFF, 7);

    /* Degenerate inputs must be refused rather than producing a broken NTB. */
    {
        uint8_t block[NCM_MAX_FRAME];
        size_t block_len = 0;
        uint8_t frame[64] = {0};

        assert(build_ntb(block, sizeof(block), frame, 0, 1, &block_len) < 0);
        assert(build_ntb(block, NCM_MIN_BLOCK - 1u, frame, 64, 1,
                         &block_len) < 0);
    }

    puts("ncm ntb conformance tests passed");
    return 0;
}
