/*
 * Unit test for the H.264 SPS frame-cropping rewriter.
 *
 * The panel mirrors the car canvas: WindowManager is sized to the canvas
 * aspect inside the 1920x1080 panel, screenrecord always captures the full
 * panel 1:1, and the black bars that leaves must never reach the sink - DiPlay
 * stretches whatever it decodes over its canvas, so a bar in the stream reads
 * as a squashed picture. Rather than rescaling (which costs a full decode and
 * encode pass), the SPS is rewritten in flight with frame_cropping offsets so
 * decoders present exactly the canvas-aspect region. This test pins that
 * rewrite: field-preserving, emulation-prevention-safe, and a no-op when the
 * requested display size already matches.
 *
 * Built and run on the host: it is a pure function over a byte buffer.
 */
#include <stdio.h>
#include <string.h>

#include "../src/airplay_h264.c"

static int failures;

#define CHECK(cond, name)                                                   \
	do {                                                                \
		if (!(cond)) {                                              \
			printf("FAIL: %s\n", name);                         \
			failures++;                                         \
		} else {                                                    \
			printf("ok: %s\n", name);                           \
		}                                                           \
	} while (0)

/* ---- bit writer for building SPS fixtures ------------------------------- */

struct bw {
	uint8_t bytes[192];
	size_t bit_len;
};

static void bw_bit(struct bw *w, int bit)
{
	size_t byte = w->bit_len >> 3;

	w->bytes[byte] &= (uint8_t)~(0x80u >> (w->bit_len & 7u));
	if (bit)
		w->bytes[byte] |= (uint8_t)(0x80u >> (w->bit_len & 7u));
	w->bit_len++;
}

static void bw_u(struct bw *w, unsigned value, int bits)
{
	int i;

	for (i = bits - 1; i >= 0; i--)
		bw_bit(w, (int)((value >> i) & 1u));
}

static void bw_ue(struct bw *w, unsigned value)
{
	unsigned encoded = value + 1u;
	int bits = 0;
	unsigned long long t = encoded;
	unsigned i;

	while (t > 1u) {
		t >>= 1;
		bits++;
	}
	for (i = 0; i < (unsigned)bits; i++)
		bw_bit(w, 0);
	for (i = 0; i <= (unsigned)bits; i++)
		bw_bit(w, (int)((encoded >> (unsigned)(bits - i)) & 1u));
}

static void bw_se(struct bw *w, int value)
{
	unsigned encoded = value <= 0 ? (unsigned)(-2 * value) :
					(unsigned)(2 * value - 1);

	bw_ue(w, encoded);
}

static void bw_trailing(struct bw *w)
{
	size_t bit = w->bit_len;

	bw_bit(w, 1);
	while (w->bit_len & 7u)
		bw_bit(w, 0);
	(void)bit;
}

/* ---- emulation prevention ------------------------------------------------ */

static size_t escape_nal(const uint8_t *rbsp, size_t rbsp_len,
			 uint8_t header_byte, uint8_t *nal, size_t cap)
{
	size_t out = 0;
	size_t i;
	int zeroes = 0;

	if (cap < 2)
		return 0;
	nal[out++] = header_byte;
	for (i = 0; i < rbsp_len && out + 1 < cap; i++) {
		if (zeroes == 2 && rbsp[i] <= 3) {
			if (out + 1 >= cap)
				return 0;
			nal[out++] = 3;
			zeroes = 0;
		}
		nal[out++] = rbsp[i];
		zeroes = rbsp[i] == 0 ? zeroes + 1 : 0;
	}
	return out;
}

static size_t unescape_nal(const uint8_t *nal, size_t nal_len,
			   uint8_t *rbsp, size_t cap)
{
	size_t out = 1; /* keep the NAL header at rbsp[0] */
	size_t i = 1;
	int zeroes = 0;

	if (!nal_len || out >= cap)
		return 0;
	rbsp[0] = nal[0];
	while (i < nal_len && out < cap) {
		if (zeroes == 2 && nal[i] == 3) {
			zeroes = 0;
			i++;
			continue;
		}
		rbsp[out++] = nal[i];
		zeroes = nal[i] == 0 ? zeroes + 1 : 0;
		i++;
	}
	return i == nal_len ? out : 0;
}

/* ---- SPS fixture builders ----------------------------------------------- */

struct sps_crop {
	unsigned left;
	unsigned right;
	unsigned top;
	unsigned bottom;
	int flag;
};

/* Parse a baseline/high-profile SPS far enough to read the crop fields back.
 * Returns 0 on success. */
static int read_se_in_test(const uint8_t *rbsp, size_t len, size_t *bit)
{
	int ue = read_ue(rbsp, len, bit);

	if (ue < 0)
		return -1;
	if (ue & 1u)
		return (ue + 1) / 2;
	return -(ue / 2);
}

static void skip_scaling_list_in_test(const uint8_t *rbsp, size_t len,
				      size_t *bit, unsigned size)
{
	int last = 8;
	int next = 8;
	unsigned j;

	for (j = 0; j < size; j++) {
		if (next != 0) {
			int delta = read_se_in_test(rbsp, len, bit);

			if (delta < -128 || delta > 127)
				return;
			next = (last + delta + 256) % 256;
		}
		if (next != 0)
			last = next;
	}
}

static int parse_sps_crop(const uint8_t *nal, size_t nal_len,
			  struct sps_crop *crop, unsigned *coded_w,
			  unsigned *coded_h)
{
	uint8_t rbsp[192];
	size_t len = unescape_nal(nal, nal_len, rbsp, sizeof(rbsp));
	size_t bit = 8; /* NAL header */
	unsigned profile_idc;
	unsigned chroma_idc = 1;
	int separate = 0;
	unsigned i;

#define READ_BIT() (bit < len * 8u ? (rbsp[bit >> 3] >> (7u - (bit & 7u))) & 1u \
				   : 0u)
#define ADVANCE() bit++
#define READ_UE() read_ue(rbsp, len, &bit)
#define FAIL() return -1

	profile_idc = 0;
	{
		int j;

		for (j = 0; j < 8; j++) {
			profile_idc = (profile_idc << 1) | (unsigned)READ_BIT();
			ADVANCE();
		}
	}
	/* constraint flags + level */
	for (i = 0; i < 16; i++) {
		(void)READ_BIT();
		ADVANCE();
	}
	(void)READ_UE(); /* seq_parameter_set_id */
	if (profile_idc == 100u || profile_idc == 110u || profile_idc == 122u ||
	    profile_idc == 244u || profile_idc == 44u || profile_idc == 83u ||
	    profile_idc == 86u || profile_idc == 118u || profile_idc == 128u ||
	    profile_idc == 138u || profile_idc == 139u || profile_idc == 134u ||
	    profile_idc == 135u) {
		int rc;
		unsigned count;
		unsigned j;

		rc = READ_UE();
		if (rc < 0)
			FAIL();
		chroma_idc = (unsigned)rc;
		if (chroma_idc == 3u) {
			separate = (int)READ_BIT();
			ADVANCE();
		}
		if (READ_UE() < 0)
			FAIL();
		if (READ_UE() < 0)
			FAIL();
		(void)READ_BIT();
		ADVANCE();
		if (READ_BIT()) { /* seq_scaling_matrix_present_flag */
			ADVANCE();
			count = chroma_idc == 3u ? 12u : 8u;
			for (j = 0; j < count; j++) {
				if (!READ_BIT())
					ADVANCE();
				else {
					ADVANCE();
					skip_scaling_list_in_test(
						rbsp, len, &bit, j < 6u ? 16u : 64u);
				}
			}
		} else {
			ADVANCE();
		}
	}
	if (READ_UE() < 0)
		FAIL();
	{
		int poc = READ_UE();

		if (poc < 0)
			FAIL();
		if (poc == 0) {
			if (READ_UE() < 0)
				FAIL();
		} else if (poc == 1) {
			if (READ_UE() < 0)
				FAIL();
		}
	}
	if (READ_UE() < 0)
		FAIL();
	(void)READ_BIT();
	ADVANCE();
	{
		int rc = READ_UE();

		if (rc < 0)
			FAIL();
		*coded_w = ((unsigned)rc + 1u) * 16u;
		rc = READ_UE();
		if (rc < 0)
			FAIL();
		*coded_h = ((unsigned)rc + 1u) * 16u;
	}
	if (!READ_BIT())
		FAIL(); /* expect frame_mbs_only_flag = 1 */
	ADVANCE();
	(void)READ_BIT();
	ADVANCE(); /* direct_8x8_inference_flag */
	crop->flag = (int)READ_BIT();
	ADVANCE();
	if (crop->flag) {
		{
			int rc = READ_UE();

			if (rc < 0)
				FAIL();
			crop->left = (unsigned)rc;
			rc = READ_UE();
			if (rc < 0)
				FAIL();
			crop->right = (unsigned)rc;
			rc = READ_UE();
			if (rc < 0)
				FAIL();
			crop->top = (unsigned)rc;
			rc = READ_UE();
			if (rc < 0)
				FAIL();
			crop->bottom = (unsigned)rc;
		}
	} else {
		crop->left = crop->right = crop->top = crop->bottom = 0;
	}
	(void)separate;
	(void)chroma_idc;
#undef READ_BIT
#undef ADVANCE
#undef READ_UE
#undef FAIL
	return 0;
}

/* Baseline SPS: coded 1920x1088 (120x68 MBs), optional pre-existing crop. */
static size_t build_sps_baseline(uint8_t *nal, size_t cap,
				 unsigned crop_right_units,
				 unsigned crop_bottom_units)
{
	struct bw w;
	uint8_t rbsp[160];

	memset(&w, 0, sizeof(w));
	bw_u(&w, 66, 8);  /* profile_idc: baseline */
	bw_u(&w, 0, 8);   /* constraint flags */
	bw_u(&w, 40, 8);  /* level_idc */
	bw_ue(&w, 0);     /* seq_parameter_set_id */
	bw_ue(&w, 0);     /* log2_max_frame_num_minus4 */
	bw_ue(&w, 2);     /* pic_order_cnt_type */
	bw_ue(&w, 1);     /* max_num_ref_frames */
	bw_bit(&w, 0);    /* gaps_in_frame_num_value_allowed_flag */
	bw_ue(&w, 119);   /* pic_width_in_mbs_minus1 -> 1920 */
	bw_ue(&w, 67);    /* pic_height_in_map_units_minus1 -> 1088 */
	bw_bit(&w, 1);    /* frame_mbs_only_flag */
	bw_bit(&w, 1);    /* direct_8x8_inference_flag */
	if (crop_right_units || crop_bottom_units) {
		bw_bit(&w, 1);
		bw_ue(&w, 0); /* left */
		bw_ue(&w, crop_right_units);
		bw_ue(&w, 0); /* top */
		bw_ue(&w, crop_bottom_units);
	} else {
		bw_bit(&w, 0);
	}
	bw_bit(&w, 0); /* vui_parameters_present_flag */
	bw_trailing(&w);
	memcpy(rbsp, w.bytes, (w.bit_len + 7u) / 8u);
	return escape_nal(rbsp, (w.bit_len + 7u) / 8u, 0x67, nal, cap);
}

/* High-profile SPS with a scaling matrix section the parser must step over. */
static size_t build_sps_high_scaling(uint8_t *nal, size_t cap)
{
	struct bw w;
	uint8_t rbsp[192];
	unsigned j;

	memset(&w, 0, sizeof(w));
	bw_u(&w, 100, 8); /* profile_idc: high */
	bw_u(&w, 0, 8);
	bw_u(&w, 40, 8);
	bw_ue(&w, 0);     /* seq_parameter_set_id */
	bw_ue(&w, 1);     /* chroma_format_idc: 4:2:0 */
	bw_ue(&w, 0);     /* bit_depth_luma_minus8 */
	bw_ue(&w, 0);     /* bit_depth_chroma_minus8 */
	bw_bit(&w, 0);    /* qpprime_y_zero_transform_bypass_flag */
	bw_bit(&w, 1);    /* seq_scaling_matrix_present_flag */
	for (j = 0; j < 8; j++) {
		bw_bit(&w, j == 0); /* only list 0 present */
		if (j == 0) {
			unsigned k;

			for (k = 0; k < 16u; k++)
				bw_se(&w, 0); /* flat 8x8 list */
		}
	}
	bw_ue(&w, 0);     /* log2_max_frame_num_minus4 */
	bw_ue(&w, 2);     /* pic_order_cnt_type */
	bw_ue(&w, 1);     /* max_num_ref_frames */
	bw_bit(&w, 0);
	bw_ue(&w, 119);
	bw_ue(&w, 67);
	bw_bit(&w, 1);
	bw_bit(&w, 1);
	bw_bit(&w, 0); /* no crop yet */
	bw_bit(&w, 0);
	bw_trailing(&w);
	memcpy(rbsp, w.bytes, (w.bit_len + 7u) / 8u);
	return escape_nal(rbsp, (w.bit_len + 7u) / 8u, 0x67, nal, cap);
}

/* ---- tests --------------------------------------------------------------- */

static void test_baseline_rewrites_bottom_crop(void)
{
	uint8_t nal[128];
	size_t len = build_sps_baseline(nal, sizeof(nal), 0u, 4u);
	struct sps_crop crop;
	unsigned coded_w = 0;
	unsigned coded_h = 0;
	int rc;

	CHECK(len > 0, "baseline fixture built");
	rc = airplay_h264_sps_apply_crop(nal, &len, sizeof(nal), 1920u, 900u);
	CHECK(rc == 0, "rewrite applied for 1920x900");
	CHECK(parse_sps_crop(nal, len, &crop, &coded_w, &coded_h) == 0,
	      "rewritten SPS parses");
	CHECK(coded_w == 1920u && coded_h == 1088u,
	      "coded macroblock size is preserved");
	CHECK(crop.flag == 1 && crop.left == 0u && crop.right == 0u &&
		  crop.top == 0u && crop.bottom == 94u,
	      "bottom crop becomes (1088-900)/2 = 94 units");
}

static void test_identity_is_a_noop(void)
{
	uint8_t nal[128];
	uint8_t before[128];
	size_t len = build_sps_baseline(nal, sizeof(nal), 0u, 4u);
	int rc;

	memcpy(before, nal, len);
	rc = airplay_h264_sps_apply_crop(nal, &len, sizeof(nal), 1920u, 1080u);
	CHECK(rc == 1, "matching display size reports no-op");
	CHECK(len == 0 || memcmp(before, nal, len) == 0,
	      "no-op leaves the NAL bytes untouched");
}

static void test_high_profile_scaling_list_survives(void)
{
	uint8_t nal[160];
	size_t len = build_sps_high_scaling(nal, sizeof(nal));
	struct sps_crop crop;
	unsigned coded_w = 0;
	unsigned coded_h = 0;

	CHECK(len > 0, "high-profile fixture built");
	CHECK(airplay_h264_sps_apply_crop(nal, &len, sizeof(nal), 1920u,
					  900u) == 0,
	      "high-profile rewrite applied");
	CHECK(parse_sps_crop(nal, len, &crop, &coded_w, &coded_h) == 0,
	      "high-profile SPS still parses");
	CHECK(crop.flag == 1 && crop.bottom == 94u && crop.right == 0u,
	      "high-profile crop offsets correct");
}

static void test_emulation_prevention_round_trips(void)
{
	/* constraint flags and level_idc both zero build a 00 00 run in the
	 * header bytes, which a real NAL must carry as 00 00 03 xx. */
	struct bw w;
	uint8_t nal[160];
	size_t len;
	struct sps_crop crop;
	unsigned coded_w = 0;
	unsigned coded_h = 0;

	memset(&w, 0, sizeof(w));
	bw_u(&w, 66, 8);
	bw_u(&w, 0, 8);   /* constraint_flags_and_reserved: 00 */
	bw_u(&w, 0, 8);   /* level_idc: 00 */
	bw_ue(&w, 254);   /* seq_parameter_set_id: long ue keeps the 00 00 run
			   * flowing into a 01 byte, so the real NAL has to
			   * carry 00 00 03 01 */
	bw_ue(&w, 0);
	bw_ue(&w, 2);
	bw_ue(&w, 1);
	bw_bit(&w, 0);
	bw_ue(&w, 119);
	bw_ue(&w, 67);
	bw_bit(&w, 1);
	bw_bit(&w, 1);
	bw_bit(&w, 0);
	bw_bit(&w, 0);
	bw_trailing(&w);
	len = escape_nal(w.bytes, (w.bit_len + 7u) / 8u, 0x67, nal,
			 sizeof(nal));
	CHECK(memchr(nal, 3, len) != NULL, "fixture really contains 00 00 03");
	CHECK(airplay_h264_sps_apply_crop(nal, &len, sizeof(nal), 1920u,
					  900u) == 0,
	      "rewrite applied on an escaped SPS");
	CHECK(parse_sps_crop(nal, len, &crop, &coded_w, &coded_h) == 0,
	      "escaped SPS parses after rewrite");
	CHECK(crop.flag == 1 && crop.bottom == 94u,
	      "crop correct after emulation-prevention round trip");
	CHECK(coded_w == 1920u && coded_h == 1088u,
	      "coded size preserved through escape/unescape");
}

static void test_truncated_sps_is_rejected(void)
{
	uint8_t nal[128];
	size_t len = build_sps_baseline(nal, sizeof(nal), 0u, 0u);
	uint8_t before[128];
	size_t short_len = 6;
	int rc;

	memcpy(before, nal, len);
	rc = airplay_h264_sps_apply_crop(nal, &short_len, sizeof(nal), 1920u,
					 900u);
	CHECK(rc == -1, "truncated SPS is rejected");
	CHECK(memcmp(before, nal, len) == 0,
	      "rejected rewrite leaves the buffer untouched");
}

static void test_oversized_display_is_rejected(void)
{
	uint8_t nal[128];
	size_t len = build_sps_baseline(nal, sizeof(nal), 0u, 0u);
	int rc;

	rc = airplay_h264_sps_apply_crop(nal, &len, sizeof(nal), 4096u, 1088u);
	CHECK(rc == -1, "display wider than the coded frame is rejected");
}

/* Verbatim SPS NAL captured from the RK3399 screenrecord encoder
 * (baseline profile, 1920x1088 coded with an existing 9-bit crop field for
 * 1080).  When the new crop field is WIDER than the stored one the rewritten
 * bitstream lands off the byte boundary; the rewrite used to fail outright
 * there, silently keeping the un-cropped SPS and letterboxing the sink's
 * picture.  The fix pads with alignment zeroes, so this fixture pins the
 * real-device behaviour. */
static void test_real_screenrecord_sps_rewrites_off_byte_boundary(void)
{
	static const uint8_t real_sps[] = {
		0x27, 0x42, 0x00, 0x1f, 0x8d, 0x68, 0x07, 0x80,
		0x22, 0x7e, 0x58, 0x40, 0x00, 0x00, 0x03, 0x00,
		0x40, 0x00, 0x00, 0x1e, 0x03, 0xc4, 0x1e, 0xa0
	};
	uint8_t nal[128];
	size_t len;
	struct sps_crop crop;
	unsigned coded_w = 0;
	unsigned coded_h = 0;

	memcpy(nal, real_sps, sizeof(real_sps));
	len = sizeof(real_sps);
	CHECK(airplay_h264_sps_apply_crop(nal, &len, sizeof(nal), 1280u,
					  600u) == 0,
	      "the real screenrecord SPS is rewritten to 1280x600");
	CHECK(len > sizeof(real_sps),
	      "a wider crop field grows the NAL");
	CHECK(parse_sps_crop(nal, len, &crop, &coded_w, &coded_h) == 0,
	      "the rewritten real SPS parses back");
	CHECK(coded_w == 1920u && coded_h == 1088u,
	      "the coded macroblock frame is untouched");
	CHECK(crop.flag == 1 && crop.left == 0u && crop.top == 0u,
	      "crop origin stays at the top-left");
	CHECK(crop.right == 320u,
	      "right crop is (1920-1280)/2 = 320 chroma units");
	CHECK(crop.bottom == 244u,
	      "bottom crop is (1088-600)/2 = 244 chroma units");
}

/* The composer renders the logical display SCALED and CENTERED inside the
 * panel (measured: wm 1280x600 composes at 0,90-1920,990 on the 1920x1080
 * panel), so the region the sink must see no longer starts at the coded
 * frame's top-left corner. The rect rewrite takes explicit origin offsets:
 * the real screenrecord SPS (coded 1920x1088) cropped to the centered
 * 1920x900 viewport must carry top=90px/bottom=90px in chroma units. */
static void test_rect_crop_removes_centered_bars(void)
{
	static const uint8_t real_sps[] = {
		0x27, 0x42, 0x00, 0x1f, 0x8d, 0x68, 0x07, 0x80,
		0x22, 0x7e, 0x58, 0x40, 0x00, 0x00, 0x03, 0x00,
		0x40, 0x00, 0x00, 0x1e, 0x03, 0xc4, 0x1e, 0xa0
	};
	uint8_t nal[128];
	size_t len;
	struct sps_crop crop;
	unsigned coded_w = 0;
	unsigned coded_h = 0;

	memcpy(nal, real_sps, sizeof(real_sps));
	len = sizeof(real_sps);
	CHECK(airplay_h264_sps_apply_crop_rect(nal, &len, sizeof(nal),
					       0u, 90u, 1920u, 900u) == 0,
	      "the real SPS is rewritten to the centered 1920x900 viewport");
	CHECK(parse_sps_crop(nal, len, &crop, &coded_w, &coded_h) == 0,
	      "the rect-rewritten SPS parses back");
	CHECK(coded_w == 1920u && coded_h == 1088u,
	      "the coded macroblock frame is untouched");
	CHECK(crop.flag == 1 && crop.left == 0u && crop.right == 0u,
	      "no horizontal bars for a full-width viewport");
	CHECK(crop.top == 45u,
	      "top bar is 90px = 45 chroma units");
	CHECK(crop.bottom == 49u,
	      "bottom bar is (1088-90-900)/2 = 49 chroma units");
}

/* A tall canvas on a wide panel leaves LEFT/RIGHT bars instead: the rewrite
 * must move the origin horizontally too (60px side bars on a 1800x1080
 * viewport, and the encoder's own bottom padding of 8px is preserved). */
static void test_rect_crop_with_horizontal_bars(void)
{
	uint8_t nal[128];
	size_t len = build_sps_baseline(nal, sizeof(nal), 0u, 0u);
	struct sps_crop crop;
	unsigned coded_w = 0;
	unsigned coded_h = 0;

	CHECK(airplay_h264_sps_apply_crop_rect(nal, &len, sizeof(nal),
					       60u, 0u, 1800u, 1080u) == 0,
	      "the SPS is rewritten to a 60px-inset 1800x1080 viewport");
	CHECK(parse_sps_crop(nal, len, &crop, &coded_w, &coded_h) == 0,
	      "the horizontally-shifted SPS parses back");
	CHECK(crop.flag == 1 && crop.left == 30u && crop.right == 30u,
	      "side bars are 60px = 30 chroma units each");
	CHECK(crop.top == 0u && crop.bottom == 4u,
	      "no top bar and the 8px encoder padding stays");
}

static void test_rect_crop_rejects_out_of_bounds(void)
{
	uint8_t nal[128];
	size_t len = build_sps_baseline(nal, sizeof(nal), 0u, 0u);
	uint8_t before[128];
	int rc;

	memcpy(before, nal, len);
	rc = airplay_h264_sps_apply_crop_rect(nal, &len, sizeof(nal),
					      0u, 90u, 1920u, 1080u);
	CHECK(rc == -1, "a rect reaching past the coded height is rejected");
	CHECK(memcmp(before, nal, len) == 0,
	      "a rejected rect leaves the buffer untouched");
	rc = airplay_h264_sps_apply_crop_rect(nal, &len, sizeof(nal),
					      0u, 0u, 0u, 900u);
	CHECK(rc == -1, "a zero-width rect is rejected");
}

static void test_parser_rect_crop_stores_origin(void)
{
	uint8_t sps[128];
	uint8_t pps[4] = { 0x68, 0xeb, 0x83, 0x32 };
	uint8_t stream[512];
	size_t stream_len = 0;
	size_t sps_len = build_sps_baseline(sps, sizeof(sps), 0u, 4u);
	struct airplay_h264_parser parser;
	uint8_t nal_buffer[512];
	struct sps_crop crop;
	unsigned coded_w = 0;
	unsigned coded_h = 0;

	memset(stream, 0, sizeof(stream));
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x01;
	memcpy(stream + stream_len, sps, sps_len);
	stream_len += sps_len;
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x01;
	memcpy(stream + stream_len, pps, sizeof(pps));
	stream_len += sizeof(pps);

	airplay_h264_parser_init(&parser, nal_buffer, sizeof(nal_buffer),
				 NULL, NULL, NULL);
	airplay_h264_parser_set_stream_crop_rect(&parser, 0u, 90u,
						 1920u, 900u);
	CHECK(airplay_h264_feed(&parser, stream, stream_len) == 0,
	      "parser feed succeeds with a rect crop configured");
	CHECK(parse_sps_crop(parser.sps, parser.sps_len, &crop, &coded_w,
			     &coded_h) == 0,
	      "stored SPS parses");
	CHECK(crop.flag == 1 && crop.top == 45u && crop.bottom == 49u &&
		  crop.left == 0u,
	      "parser rewrote the stored SPS to the centered viewport rect");
}

static void test_parser_rewrites_stored_sps(void)
{
	uint8_t sps[128];
	uint8_t pps[4] = { 0x68, 0xeb, 0x83, 0x32 };
	uint8_t stream[512];
	size_t stream_len = 0;
	size_t sps_len = build_sps_baseline(sps, sizeof(sps), 0u, 4u);
	struct airplay_h264_parser parser;
	uint8_t nal_buffer[512];
	struct sps_crop crop;
	unsigned coded_w = 0;
	unsigned coded_h = 0;

	memset(stream, 0, sizeof(stream));
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x01;
	memcpy(stream + stream_len, sps, sps_len);
	stream_len += sps_len;
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x00;
	stream[stream_len++] = 0x01;
	memcpy(stream + stream_len, pps, sizeof(pps));
	stream_len += sizeof(pps);

	airplay_h264_parser_init(&parser, nal_buffer, sizeof(nal_buffer),
				 NULL, NULL, NULL);
	airplay_h264_parser_set_stream_crop(&parser, 1920u, 900u);
	CHECK(airplay_h264_feed(&parser, stream, stream_len) == 0,
	      "parser feed succeeds with crop configured");
	/* The stored parameter set must carry the rewrite. */
	CHECK(parse_sps_crop(parser.sps, parser.sps_len, &crop, &coded_w,
			     &coded_h) == 0,
	      "stored SPS parses");
	CHECK(crop.flag == 1 && crop.bottom == 94u,
	      "parser rewrote the stored SPS to the canvas-aspect crop");
}

int main(void)
{
	test_baseline_rewrites_bottom_crop();
	test_identity_is_a_noop();
	test_high_profile_scaling_list_survives();
	test_emulation_prevention_round_trips();
	test_truncated_sps_is_rejected();
	test_oversized_display_is_rejected();
	test_real_screenrecord_sps_rewrites_off_byte_boundary();
	test_rect_crop_removes_centered_bars();
	test_rect_crop_with_horizontal_bars();
	test_rect_crop_rejects_out_of_bounds();
	test_parser_rect_crop_stores_origin();
	test_parser_rewrites_stored_sps();
	if (failures) {
		printf("sps crop tests failed: %d\n", failures);
		return 1;
	}
	printf("sps crop tests passed\n");
	return 0;
}
