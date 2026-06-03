/*
 * test_zran.c
 *
 * Slice 060 fork: validate the zran-style resume + walk entrypoints.
 *
 * Test plan:
 *   1. Build a moderately-compressible payload (~256 KiB).
 *   2. Compress as raw DEFLATE.
 *   3. Decompress normally to obtain the canonical output.
 *   4. Decompress again via libdeflate_deflate_decompress_walk()
 *      in a loop with stop_at_block_end=1, threading
 *      (bitbuf, bitsleft) AND the last 32 KiB of output as a
 *      dictionary between calls. Capture each checkpoint.
 *   5. From the first captured checkpoint (which has its own
 *      window snapshot), call libdeflate_deflate_decompress_resume()
 *      to decompress the remainder; verify the resumed output
 *      matches the canonical bytes starting at the checkpoint's
 *      output position.
 */

#include "test_util.h"

#include <string.h>

#define PAYLOAD_SIZE  (256 * 1024)
#define WINDOW_SIZE   (32 * 1024)

static void
fill_payload(u8 *buf, size_t n)
{
	size_t i;
	const u8 template[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x42, 0x17 };
	for (i = 0; i < n; i++)
		buf[i] = template[i & 7] ^ (u8)(i >> 5);
}

/* Snapshot the last (up to) WINDOW_SIZE bytes of decoded output
 * ending at `pos` (absolute position in canonical output space)
 * into `window`, returning the count of valid bytes from the END
 * of `window`. */
static size_t
snapshot_window(const u8 *full_output, size_t pos, u8 *window)
{
	size_t take = pos < WINDOW_SIZE ? pos : WINDOW_SIZE;
	memset(window, 0, WINDOW_SIZE - take);
	memcpy(window + (WINDOW_SIZE - take),
	       full_output + pos - take, take);
	return take;
}

int
tmain(int argc, tchar *argv[])
{
	struct libdeflate_compressor *c;
	struct libdeflate_decompressor *d;
	u8 *payload, *compressed, *canonical, *via_walk, *via_resume;
	size_t compressed_size, canonical_out, actual_out;

	payload    = xmalloc(PAYLOAD_SIZE);
	compressed = xmalloc(PAYLOAD_SIZE);
	canonical  = xmalloc(PAYLOAD_SIZE);
	via_walk   = xmalloc(PAYLOAD_SIZE);
	via_resume = xmalloc(PAYLOAD_SIZE + WINDOW_SIZE);
	fill_payload(payload, PAYLOAD_SIZE);

	c = libdeflate_alloc_compressor(6);
	ASSERT(c != NULL);
	compressed_size = libdeflate_deflate_compress(c, payload, PAYLOAD_SIZE,
						      compressed, PAYLOAD_SIZE);
	ASSERT(compressed_size > 0);
	ASSERT(compressed_size < PAYLOAD_SIZE);
	libdeflate_free_compressor(c);

	d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);

	ASSERT(libdeflate_deflate_decompress(d, compressed, compressed_size,
					     canonical, PAYLOAD_SIZE,
					     &canonical_out) == LIBDEFLATE_SUCCESS);
	ASSERT(canonical_out == PAYLOAD_SIZE);
	ASSERT(memcmp(canonical, payload, PAYLOAD_SIZE) == 0);

	/* Walk the stream block-by-block, threading state. */
	{
		uint64_t bitbuf = 0;
		uint32_t bitsleft = 0;
		u8 dict[WINDOW_SIZE];
		size_t dict_used = 0;
		size_t in_consumed, out_produced;
		enum libdeflate_result r;
		size_t in_pos = 0, out_pos = 0;
		int saw_block_end = 0;
		uint64_t saved_bitbuf = 0;
		uint32_t saved_bitsleft = 0;
		size_t saved_in_pos = 0, saved_out_pos = 0;
		u8 saved_window[WINDOW_SIZE];
		size_t saved_window_used = 0;
		int iter = 0;

		(void)dict;
		(void)dict_used;

		while (in_pos < compressed_size) {
			r = libdeflate_deflate_decompress_walk(
				d,
				compressed + in_pos, compressed_size - in_pos,
				via_walk, PAYLOAD_SIZE,
				bitbuf, bitsleft,
				/* out_offset = */ out_pos,
				/* stop_at_block_end = */ 1,
				&bitbuf, &bitsleft,
				&in_consumed, &out_produced);

			in_pos  += in_consumed;
			out_pos += out_produced;

			if (r == LIBDEFLATE_BLOCK_END) {
				if (!saw_block_end) {
					saved_window_used =
						snapshot_window(via_walk, out_pos, saved_window);
					saved_bitbuf   = bitbuf;
					saved_bitsleft = bitsleft;
					saved_in_pos   = in_pos;
					saved_out_pos  = out_pos;
					saw_block_end  = 1;
				}
				iter++;
				continue;
			}
			if (r == LIBDEFLATE_SUCCESS)
				break;
			ASSERT(0 && "walk failed mid-stream");
		}
		ASSERT(out_pos == PAYLOAD_SIZE);
		ASSERT(memcmp(via_walk, payload, PAYLOAD_SIZE) == 0);
		ASSERT(saw_block_end && "expected at least one end-of-block");

		/* Resume from the captured checkpoint. */
		{
			size_t in_left = compressed_size - saved_in_pos;
			const u8 *dict_p = saved_window_used > 0
				? saved_window + (WINDOW_SIZE - saved_window_used)
				: NULL;

			r = libdeflate_deflate_decompress_resume(
				d,
				compressed + saved_in_pos, in_left,
				via_resume, PAYLOAD_SIZE + WINDOW_SIZE,
				saved_bitbuf, saved_bitsleft,
				dict_p, saved_window_used,
				NULL, &actual_out);
			ASSERT(r == LIBDEFLATE_SUCCESS);
			ASSERT(actual_out == PAYLOAD_SIZE - saved_out_pos);
			ASSERT(memcmp(via_resume + saved_window_used,
				      payload + saved_out_pos,
				      actual_out) == 0);
		}
	}

	libdeflate_free_decompressor(d);
	free(payload);
	free(compressed);
	free(canonical);
	free(via_walk);
	free(via_resume);

	printf("test_zran: walk + resume round-trip ok\n");
	return 0;
}
