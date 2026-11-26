/* Host-side dump of the exact NTB bytes produced by src/ncm_bridge.c.
 * Decodes the result strictly per CDC-NCM 1.0 (NTB-16 + NDP-16) so we can
 * see what a standard host driver would actually accept. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NCM_NTH16_BYTES  12u
#define NCM_NDP16_BYTES  16u
#define NCM_NTH16_SIG    0x484d434eu
#define NCM_NDP16_SIG    0x304d434eu
#define NCM_DATAGRAM_OFF 28u

static void wr16_le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32_le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t rd16_le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32_le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Verbatim copy of ncm_bridge.c:build_ntb() */
static int build_ntb(uint8_t *out, size_t cap, const uint8_t *frame,
		     size_t frame_len, uint16_t sequence, size_t *out_len)
{
	size_t total = NCM_DATAGRAM_OFF + frame_len;

	if (!frame_len || total > 0xffffu || total > cap)
		return -1;
	memset(out, 0, NCM_DATAGRAM_OFF);
	wr32_le(out, NCM_NTH16_SIG);
	wr16_le(out + 4, NCM_NTH16_BYTES);
	wr16_le(out + 6, sequence);
	wr16_le(out + 8, (uint16_t)total);
	wr16_le(out + 10, NCM_NTH16_BYTES);
	wr32_le(out + 12, NCM_NDP16_SIG);
	wr16_le(out + 16, NCM_NDP16_BYTES);
	wr16_le(out + 18, 0);
	wr16_le(out + 20, NCM_DATAGRAM_OFF);
	wr16_le(out + 22, (uint16_t)frame_len);
	wr16_le(out + 24, 0);
	wr16_le(out + 26, 0);
	memcpy(out + NCM_DATAGRAM_OFF, frame, frame_len);
	*out_len = total;
	return 0;
}

static void hexdump(const char *tag, const uint8_t *p, size_t n)
{
	size_t i;

	printf("%s (%u bytes):\n  ", tag, (unsigned)n);
	for (i = 0; i < n && i < 48; i++)
		printf("%02x ", p[i]);
	printf("\n");
}

int main(void)
{
	uint8_t frame[1510];
	uint8_t block[65535];
	size_t block_len = 0;

	memset(frame, 0xab, sizeof(frame));
	/* plausible ethernet header: dst, src, ethertype 0x86dd */
	memset(frame, 0x0e, 6);
	memset(frame + 6, 0x72, 6);
	frame[12] = 0x86; frame[13] = 0xdd;

	if (build_ntb(block, sizeof(block), frame, sizeof(frame), 1, &block_len) < 0) {
		printf("build_ntb failed\n");
		return 1;
	}

	hexdump("NTB head", block, block_len < 48 ? block_len : 48);

	uint32_t sig      = rd32_le(block);
	uint16_t hdr_len  = rd16_le(block + 4);
	uint16_t seq      = rd16_le(block + 6);
	uint16_t blk_len  = rd16_le(block + 8);
	uint16_t ndp_idx  = rd16_le(block + 10);

	printf("\n-- NTH16 --\n");
	printf("signature   = 0x%08x %s\n", sig,
	       sig == NCM_NTH16_SIG ? "(OK 'NCMH')" : "(BAD)");
	printf("header_len  = %u (expect 12)\n", hdr_len);
	printf("sequence    = %u\n", seq);
	printf("block_len   = %u (expect %u)\n", blk_len, (unsigned)block_len);
	printf("ndp_index   = %u (expect 12)\n\n", ndp_idx);

	const uint8_t *ndp = block + ndp_idx;
	uint32_t nsig     = rd32_le(ndp);
	uint16_t nlen     = rd16_le(ndp + 4);
	uint16_t nnext    = rd16_le(ndp + 6);
	uint32_t dgram_ix = rd32_le(ndp + 8);   /* entry[0].wDatagramIndex, 4 bytes */
	uint16_t dgram_ln = rd16_le(ndp + 12);  /* entry[0].wDatagramLength, 2 bytes */
	uint16_t dgram_nx = rd16_le(ndp + 14);

	printf("-- NDP16 --\n");
	printf("signature   = 0x%08x %s\n", nsig,
	       (nsig & 0x00ffffffu) == (NCM_NDP16_SIG & 0x00ffffffu) ? "(OK 'ncm0')" : "(BAD)");
	printf("length      = %u (expect 16)\n", nlen);
	printf("next_ndp    = %u (expect 0)\n", nnext);
	printf("entry[0].wDatagramIndex  = %u (expect 28)  %s\n", dgram_ix,
	       dgram_ix == NCM_DATAGRAM_OFF ? "OK" : "*** WRONG ***");
	printf("entry[0].wDatagramLength = %u (expect %u) %s\n", dgram_ln, (unsigned)sizeof(frame),
	       dgram_ln == sizeof(frame) ? "OK" : "*** WRONG ***");
	printf("entry[0].wNextNdpIndex   = %u (expect 0)\n\n", dgram_nx);

	/* What cdc_ncm.c does: walk entries, stop at length==0 or index==0. */
	printf("-- host-side simulation (cdc_ncm_rx_fixup) --\n");
	if (dgram_ln == 0)
		printf("entry length is 0 -> driver STOPS, accepts 0 datagrams from this NTB\n");
	else if (dgram_ix + dgram_ln > blk_len)
		printf("entry out of range -> driver drops the NTB\n");
	else
		printf("1 datagram accepted at offset %u len %u\n", dgram_ix, dgram_ln);
	return 0;
}
