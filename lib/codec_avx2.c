#include <stdint.h>
#include <stddef.h>

#include "../include/libbase64.h"
#include "codecs.h"

#ifdef __AVX2__
#include <immintrin.h>

#define CMPGT(s,n)	_mm256_cmpgt_epi8((s), _mm256_set1_epi8(n))
#define CMPEQ(s,n)	_mm256_cmpeq_epi8((s), _mm256_set1_epi8(n))
#define REPLACE(s,n)	_mm256_and_si256((s), _mm256_set1_epi8(n))
#define RANGE(s,a,b)	_mm256_andnot_si256(CMPGT((s), (b)), CMPGT((s), (a) - 1))

static inline __m256i
_mm256_bswap_epi32 (const __m256i in)
{
	// _mm256_shuffle_epi8() works on two 128-bit lanes separately:
	return _mm256_shuffle_epi8(in, _mm256_setr_epi8(
		 3,  2,  1,  0,
		 7,  6,  5,  4,
		11, 10,  9,  8,
		15, 14, 13, 12,
		 3,  2,  1,  0,
		 7,  6,  5,  4,
		11, 10,  9,  8,
		15, 14, 13, 12));
}

static inline __m256i
enc_reshuffle (__m256i in)
{
	// Spread out 32-bit words over both halves of the input register:
	in = _mm256_permutevar8x32_epi32(in, _mm256_setr_epi32(
		0, 1, 2, -1,
		3, 4, 5, -1));

	// For each 32-bit word, reorder to bigendian, duplicating the third
	// byte in every block of four:
	in = _mm256_shuffle_epi8(in, _mm256_setr_epi8(
		 2,  2,  1,  0,
		 5,  5,  4,  3,
		 8,  8,  7,  6,
		11, 11, 10,  9,
		 2,  2,  1,  0,
		 5,  5,  4,  3,
		 8,  8,  7,  6,
		11, 11, 10,  9));

	// Mask to pass through only the lower 6 bits of one byte:
	__m256i mask = _mm256_set1_epi32(0x3F000000);

	// Shift bits by 2, mask in only the first byte:
	__m256i out = _mm256_and_si256(_mm256_srli_epi32(in, 2), mask);
	mask = _mm256_srli_epi32(mask, 8);

	// Shift bits by 4, mask in only the second byte:
	out = _mm256_or_si256(out, _mm256_and_si256(_mm256_srli_epi32(in, 4), mask));
	mask = _mm256_srli_epi32(mask, 8);

	// Shift bits by 6, mask in only the third byte:
	out = _mm256_or_si256(out, _mm256_and_si256(_mm256_srli_epi32(in, 6), mask));
	mask = _mm256_srli_epi32(mask, 8);

	// No shift necessary for the fourth byte because we duplicated the
	// third byte to this position; just mask:
	out = _mm256_or_si256(out, _mm256_and_si256(in, mask));

	// Reorder to 32-bit little-endian:
	return _mm256_bswap_epi32(out);
}

static inline __m256i
enc_translate (const __m256i in)
{
	// Translate values 0..63 to the Base64 alphabet. There are five sets:
	// #  From      To         Abs  Delta  Characters
	// 0  [0..25]   [65..90]   +65  +65    ABCDEFGHIJKLMNOPQRSTUVWXYZ
	// 1  [26..51]  [97..122]  +71   +6    abcdefghijklmnopqrstuvwxyz
	// 2  [52..61]  [48..57]    -4  -75    0123456789
	// 3  [62]      [43]       -19  -15    +
	// 4  [63]      [47]       -16   +3    /

	// Create cumulative masks for characters in sets [1,2,3,4], [2,3,4],
	// [3,4], and [4]:
	const __m256i mask1 = CMPGT(in, 25);
	const __m256i mask2 = CMPGT(in, 51);
	const __m256i mask3 = CMPGT(in, 61);
	const __m256i mask4 = CMPEQ(in, 63);

	// All characters are at least in cumulative set 0, so add 'A':
	__m256i out = _mm256_add_epi8(in, _mm256_set1_epi8(65));

	// For inputs which are also in any of the other cumulative sets,
	// add delta values against the previous set(s) to correct the shift:
	out = _mm256_add_epi8(out, REPLACE(mask1,  6));
	out = _mm256_sub_epi8(out, REPLACE(mask2, 75));
	out = _mm256_sub_epi8(out, REPLACE(mask3, 15));
	out = _mm256_add_epi8(out, REPLACE(mask4,  3));

	return out;
}

static inline __m256i
dec_reshuffle (__m256i in)
{
	// Shuffle bytes to 32-bit bigendian:
	in = _mm256_bswap_epi32(in);

	// Mask in a single byte per shift:
	__m256i mask = _mm256_set1_epi32(0x3F000000);

	// Pack bytes together:
	__m256i out = _mm256_slli_epi32(_mm256_and_si256(in, mask), 2);
	mask = _mm256_srli_epi32(mask, 8);

	out = _mm256_or_si256(out, _mm256_slli_epi32(_mm256_and_si256(in, mask), 4));
	mask = _mm256_srli_epi32(mask, 8);

	out = _mm256_or_si256(out, _mm256_slli_epi32(_mm256_and_si256(in, mask), 6));
	mask = _mm256_srli_epi32(mask, 8);

	out = _mm256_or_si256(out, _mm256_slli_epi32(_mm256_and_si256(in, mask), 8));

	// Pack bytes together within 32-bit words, discarding words 3 and 7:
	out = _mm256_shuffle_epi8(out, _mm256_setr_epi8(
		 3,  2,  1,
		 7,  6,  5,
		11, 10,  9,
		15, 14, 13,
		-1, -1, -1, -1,
		 3,  2,  1,
		 7,  6,  5,
		11, 10,  9,
		15, 14, 13,
		-1, -1, -1, -1));

	// Pack 32-bit words together, squashing empty words 3 and 7:
	return _mm256_permutevar8x32_epi32(out, _mm256_setr_epi32(
		0, 1, 2, 4, 5, 6, -1, -1));
}

#endif	// __AVX2__

BASE64_ENC_FUNCTION(avx2)
{
#ifdef __AVX2__
	#include "enc/head.c"
	#include "enc/avx2.c"
	#include "enc/tail.c"
#else
	BASE64_ENC_STUB
#endif
}

BASE64_DEC_FUNCTION(avx2)
{
#ifdef __AVX2__
	#include "dec/head.c"
	#include "dec/avx2.c"
	#include "dec/tail.c"
#else
	BASE64_DEC_STUB
#endif
}
