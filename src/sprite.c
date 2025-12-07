/* * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) 2007-2009  The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "sprite.h"

#include "file.h"
#include "opentyr.h"
#include "video.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

 /* SIMD / Optimization Headers */
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#include <immintrin.h>
#elif defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#endif

Sprite_array sprite_table[SPRITE_TABLES_MAX];

Sprite2_array shopSpriteSheet;

Sprite2_array explosionSpriteSheet;

Sprite2_array enemySpriteSheets[4];
Uint8 enemySpriteSheetIds[4];

Sprite2_array destructSpriteSheet;

Sprite2_array spriteSheet8;
Sprite2_array spriteSheet9;
Sprite2_array spriteSheet10;
Sprite2_array spriteSheet11;
Sprite2_array spriteSheet12;
Sprite2_array spriteSheetT2000;

/* --- Optimization Globals and Detection --- */

static int g_hasAVX512 = -1; // -1 = unknown, 0 = no, 1 = yes

static void JE_detectAVX512(void)
{
	if (g_hasAVX512 != -1) return;

	g_hasAVX512 = 0;

#if defined(__GNUC__) || defined(__clang__)
	unsigned int eax, ebx, ecx, edx;
	// Check for AVX-512 F (Foundation) and BW (Byte/Word)
	// __get_cpuid_count(level, count, eax, ebx, ecx, edx)
	// Level 7, Subleaf 0. EBX bit 16 = AVX512F, bit 30 = AVX512BW
	if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
		if ((ebx & (1 << 16)) && (ebx & (1 << 30))) {
			g_hasAVX512 = 1;
		}
	}
#elif defined(_MSC_VER)
	int regs[4];
	__cpuidex(regs, 7, 0);
	if ((regs[1] & (1 << 16)) && (regs[1] & (1 << 30))) {
		g_hasAVX512 = 1;
	}
#endif
}

/* --- Loader Functions --- */

void load_sprites_file(unsigned int table, const char* filename)
{
	free_sprites(table);

	FILE* f = dir_fopen_die(data_dir(), filename, "rb");

	load_sprites(table, f);

	fclose(f);
}

void load_sprites(unsigned int table, FILE* f)
{
	free_sprites(table);

	Uint16 temp;
	fread_u16_die(&temp, 1, f);

	sprite_table[table].count = temp;

	assert(sprite_table[table].count <= SPRITES_PER_TABLE_MAX);

	for (unsigned int i = 0; i < sprite_table[table].count; ++i)
	{
		Sprite* const cur_sprite = sprite(table, i);

		bool populated;
		fread_bool_die(&populated, f);
		if (!populated) // sprite is empty
			continue;

		fread_u16_die(&cur_sprite->width, 1, f);
		fread_u16_die(&cur_sprite->height, 1, f);
		fread_u16_die(&cur_sprite->size, 1, f);

		cur_sprite->data = malloc(cur_sprite->size);

		fread_u8_die(cur_sprite->data, cur_sprite->size, f);
	}

	// Lazy init optimization detection on first load
	JE_detectAVX512();
}

void free_sprites(unsigned int table)
{
	for (unsigned int i = 0; i < sprite_table[table].count; ++i)
	{
		Sprite* const cur_sprite = sprite(table, i);

		cur_sprite->width = 0;
		cur_sprite->height = 0;
		cur_sprite->size = 0;

		free(cur_sprite->data);
		cur_sprite->data = NULL;
	}

	sprite_table[table].count = 0;
}

// does not clip on left or right edges of surface
void blit_sprite(SDL_Surface* surface, int x, int y, unsigned int table, unsigned int index)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index))
	{
		assert(false);
		return;
	}

	const Sprite* const cur_sprite = sprite(table, index);

	const Uint8* data = cur_sprite->data;
	const Uint8* const data_ul = data + cur_sprite->size;

	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;

	assert(surface->format->BitsPerPixel == 8);
	Uint8* pixels = (Uint8*)surface->pixels + (y * surface->pitch) + x;
	const Uint8* const pixels_ll = (Uint8*)surface->pixels,  // lower limit
		* const pixels_ul = (Uint8*)surface->pixels + (surface->h * surface->pitch);  // upper limit

	for (; data < data_ul; ++data)
	{
		switch (*data)
		{
		case 255:  // transparent pixels
			data++;  // next byte tells how many
			pixels += *data;
			x_offset += *data;
			break;

		case 254:  // next pixel row
			pixels += width - x_offset;
			x_offset = width;
			break;

		case 253:  // 1 transparent pixel
			pixels++;
			x_offset++;
			break;

		default:  // set a pixel
			// OPTIMIZED: Run-ahead detection for standard sprites
		{
			if (pixels >= pixels_ul) return;

			// Write current
			if (pixels >= pixels_ll) *pixels = *data;
			pixels++;
			x_offset++;

			// Look ahead for solid run to avoid switch overhead
			// We stop at width boundary or if we hit control codes (>= 253)
			while (x_offset < width && data[1] < 253 && (data + 1) < data_ul)
			{
				data++;
				if (pixels >= pixels_ll && pixels < pixels_ul)
					*pixels = *data;
				pixels++;
				x_offset++;
			}
		}
		break;
		}
		if (x_offset >= width)
		{
			pixels += surface->pitch - x_offset;
			x_offset = 0;
		}
	}
}

void blit_sprite_blend(SDL_Surface* surface, int x, int y, unsigned int table, unsigned int index)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index))
	{
		assert(false);
		return;
	}

	const Sprite* const cur_sprite = sprite(table, index);
	const Uint8* data = cur_sprite->data;
	const Uint8* const data_ul = data + cur_sprite->size;
	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;
	assert(surface->format->BitsPerPixel == 8);
	Uint8* pixels = (Uint8*)surface->pixels + (y * surface->pitch) + x;
	const Uint8* const pixels_ll = (Uint8*)surface->pixels;
	const Uint8* const pixels_ul = (Uint8*)surface->pixels + (surface->h * surface->pitch);

	for (; data < data_ul; ++data) {
		switch (*data) {
		case 255: data++; pixels += *data; x_offset += *data; break;
		case 254: pixels += width - x_offset; x_offset = width; break;
		case 253: pixels++; x_offset++; break;
		default:
			if (pixels >= pixels_ul) return;
			if (pixels >= pixels_ll)
				*pixels = (*data & 0xf0) | (((*pixels & 0x0f) + (*data & 0x0f)) / 2);
			pixels++; x_offset++; break;
		}
		if (x_offset >= width) {
			pixels += surface->pitch - x_offset;
			x_offset = 0;
		}
	}
}

void blit_sprite_hv_unsafe(SDL_Surface* surface, int x, int y, unsigned int table, unsigned int index, Uint8 hue, Sint8 value)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index)) { assert(false); return; }
	hue <<= 4;
	const Sprite* const cur_sprite = sprite(table, index);
	const Uint8* data = cur_sprite->data;
	const Uint8* const data_ul = data + cur_sprite->size;
	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;
	assert(surface->format->BitsPerPixel == 8);
	Uint8* pixels = (Uint8*)surface->pixels + (y * surface->pitch) + x;
	const Uint8* const pixels_ll = (Uint8*)surface->pixels;
	const Uint8* const pixels_ul = (Uint8*)surface->pixels + (surface->h * surface->pitch);

	for (; data < data_ul; ++data) {
		switch (*data) {
		case 255: data++; pixels += *data; x_offset += *data; break;
		case 254: pixels += width - x_offset; x_offset = width; break;
		case 253: pixels++; x_offset++; break;
		default:
			if (pixels >= pixels_ul) return;
			if (pixels >= pixels_ll) *pixels = hue | ((*data & 0x0f) + value);
			pixels++; x_offset++; break;
		}
		if (x_offset >= width) { pixels += surface->pitch - x_offset; x_offset = 0; }
	}
}

void blit_sprite_hv(SDL_Surface* surface, int x, int y, unsigned int table, unsigned int index, Uint8 hue, Sint8 value)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index)) { assert(false); return; }
	hue <<= 4;
	const Sprite* const cur_sprite = sprite(table, index);
	const Uint8* data = cur_sprite->data;
	const Uint8* const data_ul = data + cur_sprite->size;
	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;
	assert(surface->format->BitsPerPixel == 8);
	Uint8* pixels = (Uint8*)surface->pixels + (y * surface->pitch) + x;
	const Uint8* const pixels_ll = (Uint8*)surface->pixels;
	const Uint8* const pixels_ul = (Uint8*)surface->pixels + (surface->h * surface->pitch);

	for (; data < data_ul; ++data) {
		switch (*data) {
		case 255: data++; pixels += *data; x_offset += *data; break;
		case 254: pixels += width - x_offset; x_offset = width; break;
		case 253: pixels++; x_offset++; break;
		default:
			if (pixels >= pixels_ul) return;
			if (pixels >= pixels_ll) {
				Uint8 temp_value = (*data & 0x0f) + value;
				if (temp_value > 0xf) temp_value = (temp_value >= 0x1f) ? 0x0 : 0xf;
				*pixels = hue | temp_value;
			}
			pixels++; x_offset++; break;
		}
		if (x_offset >= width) { pixels += surface->pitch - x_offset; x_offset = 0; }
	}
}

void blit_sprite_hv_blend(SDL_Surface* surface, int x, int y, unsigned int table, unsigned int index, Uint8 hue, Sint8 value)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index)) { assert(false); return; }
	hue <<= 4;
	const Sprite* const cur_sprite = sprite(table, index);
	const Uint8* data = cur_sprite->data;
	const Uint8* const data_ul = data + cur_sprite->size;
	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;
	assert(surface->format->BitsPerPixel == 8);
	Uint8* pixels = (Uint8*)surface->pixels + (y * surface->pitch) + x;
	const Uint8* const pixels_ll = (Uint8*)surface->pixels;
	const Uint8* const pixels_ul = (Uint8*)surface->pixels + (surface->h * surface->pitch);

	for (; data < data_ul; ++data) {
		switch (*data) {
		case 255: data++; pixels += *data; x_offset += *data; break;
		case 254: pixels += width - x_offset; x_offset = width; break;
		case 253: pixels++; x_offset++; break;
		default:
			if (pixels >= pixels_ul) return;
			if (pixels >= pixels_ll) {
				Uint8 temp_value = (*data & 0x0f) + value;
				if (temp_value > 0xf) temp_value = (temp_value >= 0x1f) ? 0x0 : 0xf;
				*pixels = hue | (((*pixels & 0x0f) + temp_value) / 2);
			}
			pixels++; x_offset++; break;
		}
		if (x_offset >= width) { pixels += surface->pitch - x_offset; x_offset = 0; }
	}
}

void blit_sprite_dark(SDL_Surface* surface, int x, int y, unsigned int table, unsigned int index, bool black)
{
	if (index >= sprite_table[table].count || !sprite_exists(table, index)) { assert(false); return; }
	const Sprite* const cur_sprite = sprite(table, index);
	const Uint8* data = cur_sprite->data;
	const Uint8* const data_ul = data + cur_sprite->size;
	const unsigned int width = cur_sprite->width;
	unsigned int x_offset = 0;
	assert(surface->format->BitsPerPixel == 8);
	Uint8* pixels = (Uint8*)surface->pixels + (y * surface->pitch) + x;
	const Uint8* const pixels_ll = (Uint8*)surface->pixels;
	const Uint8* const pixels_ul = (Uint8*)surface->pixels + (surface->h * surface->pitch);

	for (; data < data_ul; ++data) {
		switch (*data) {
		case 255: data++; pixels += *data; x_offset += *data; break;
		case 254: pixels += width - x_offset; x_offset = width; break;
		case 253: pixels++; x_offset++; break;
		default:
			if (pixels >= pixels_ul) return;
			if (pixels >= pixels_ll)
				*pixels = black ? 0x00 : ((*pixels & 0xf0) | ((*pixels & 0x0f) / 2));
			pixels++; x_offset++; break;
		}
		if (x_offset >= width) { pixels += surface->pitch - x_offset; x_offset = 0; }
	}
}

/* --- Sprite2 (Compressed Shapes) Loader --- */

void JE_loadCompShapes(Sprite2_array* sprite2s, char s)
{
	free_sprite2s(sprite2s);

	char buffer[20];
	snprintf(buffer, sizeof(buffer), "newsh%c.shp", tolower((unsigned char)s));

	FILE* f = dir_fopen_die(data_dir(), buffer, "rb");

	sprite2s->size = ftell_eof(f);

	JE_loadCompShapesB(sprite2s, f);

	fclose(f);
}

void JE_loadCompShapesB(Sprite2_array* sprite2s, FILE* f)
{
	assert(sprite2s->data == NULL);

	sprite2s->data = malloc(sprite2s->size);
	fread_u8_die(sprite2s->data, sprite2s->size, f);
}

void free_sprite2s(Sprite2_array* sprite2s)
{
	free(sprite2s->data);
	sprite2s->data = NULL;

	sprite2s->size = 0;
}

/* --- OPTIMIZED BLITTERS --- */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw")))
#endif
static void blit_sprite2_AVX512(Uint8* pixels, const Uint8* data,
	const Uint8* pixels_ll, const Uint8* pixels_ul,
	int pitch)
{
	// Process the RLE stream
	for (; *data != 0x0f; ++data)
	{
		Uint8 control = *data;
		int skip = control & 0x0f;
		int count = (control & 0xf0) >> 4;

		pixels += skip;

		if (count == 0) // New Line
		{
			pixels += pitch - 12; // 12 is standard width decrement? 
			// Note: Original code assumes specific width logic here (12px standard blocks)
		}
		else
		{
			data++; // Point to pixel data

			// Check bounds safely
			if (pixels >= pixels_ll && (pixels + count) <= pixels_ul)
			{
				const Uint8* src = data;
				Uint8* dst = pixels;
				int n = count;

				// AVX-512 Loop
				// We use Masked Stores. This handles alignment and variable lengths (tails) perfectly.
				while (n > 0)
				{
					// Mask for n pixels (up to 64)
					// We use ~0ULL instead of -1ULL to avoid MSVC error C4146 (unary minus on unsigned)
					__mmask64 mask = (n >= 64) ? ~0ULL : (1ULL << n) - 1;

					// Load src (unaligned)
					__m512i v = _mm512_maskz_loadu_epi8(mask, src);

					// Store dst (unaligned)
					_mm512_mask_storeu_epi8(dst, mask, v);

					src += 64;
					dst += 64;
					n -= 64;
				}

				pixels += count;
				data += count - 1; // Loop increments data, so adjust
			}
			else
			{
				// Fallback for clipping
				while (count--) {
					if (pixels >= pixels_ll && pixels < pixels_ul) *pixels = *data;
					pixels++; data++;
				}
				data--;
			}
		}
	}
}

// Standard fallback using memcpy/unrolling
static void blit_sprite2_generic(Uint8* pixels, const Uint8* data,
	const Uint8* pixels_ll, const Uint8* pixels_ul,
	int pitch)
{
	for (; *data != 0x0f; ++data)
	{
		pixels += *data & 0x0f;
		unsigned int count = (*data & 0xf0) >> 4;

		if (count == 0) pixels += pitch - 12;
		else
		{
			data++;
			// Optimization: Memcpy for valid runs
			if (pixels >= pixels_ll && (pixels + count) <= pixels_ul) {
				memcpy(pixels, data, count);
				pixels += count;
				data += count - 1;
			}
			else {
				while (count--) {
					if (pixels >= pixels_ll && pixels < pixels_ul) *pixels = *data;
					++pixels; ++data;
				}
				data--;
			}
		}
	}
}

void blit_sprite2(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	assert(surface->format->BitsPerPixel == 8);
	Uint8* pixels = (Uint8*)surface->pixels + (y * surface->pitch) + x;
	const Uint8* pixels_ll = (Uint8*)surface->pixels;
	const Uint8* pixels_ul = (Uint8*)surface->pixels + (surface->h * surface->pitch);
	const Uint8* data = sprite2s.data + SDL_SwapLE16(((Uint16*)sprite2s.data)[index - 1]);

	// Dispatch
	if (g_hasAVX512 == 1) {
		blit_sprite2_AVX512(pixels, data, pixels_ll, pixels_ul, VGAScreen->pitch);
	}
	else {
		blit_sprite2_generic(pixels, data, pixels_ll, pixels_ul, VGAScreen->pitch);
	}
}

void blit_sprite2_clip(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	// Clip logic is complex to vectorize due to coordinate checks per run
	// Using standard logic
	assert(surface->format->BitsPerPixel == 8);
	const Uint8* data = sprite2s.data + SDL_SwapLE16(((Uint16*)sprite2s.data)[index - 1]);

	for (; *data != 0x0f; ++data)
	{
		if (y >= surface->h) return;
		x += *data & 0x0f;
		Uint8 fill_count = (*data >> 4) & 0x0f;

		if (fill_count == 0) { y += 1; x -= 12; }
		else if (y >= 0)
		{
			Uint8* const pixel_row = (Uint8*)surface->pixels + (y * surface->pitch);
			do {
				++data;
				if (x >= 0 && x < surface->pitch) pixel_row[x] = *data;
				x += 1;
			} while (--fill_count);
		}
		else { data += fill_count; x += fill_count; }
	}
}

/* --- BLENDING OPTIMIZATION --- */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw")))
#endif
static void blit_sprite2_blend_AVX512(Uint8* pixels, const Uint8* data,
	const Uint8* pixels_ll, const Uint8* pixels_ul,
	int pitch)
{
	// Logic: (((src & 0xF) + (dst & 0xF)) / 2) | (src & 0xF0)
	__m512i maskF = _mm512_set1_epi8(0x0F);
	__m512i maskF0 = _mm512_set1_epi8(0xF0);

	for (; *data != 0x0f; ++data)
	{
		pixels += *data & 0x0f;
		int count = (*data & 0xf0) >> 4;

		if (count == 0) pixels += pitch - 12;
		else
		{
			data++;
			if (pixels >= pixels_ll && (pixels + count) <= pixels_ul)
			{
				const Uint8* src = data;
				Uint8* dst = pixels;
				int n = count;

				while (n > 0)
				{
					// We use ~0ULL instead of -1ULL to avoid MSVC error C4146
					__mmask64 mask = (n >= 64) ? ~0ULL : (1ULL << n) - 1;

					__m512i vSrc = _mm512_maskz_loadu_epi8(mask, src);
					__m512i vDst = _mm512_maskz_loadu_epi8(mask, dst);

					// src & 0x0F
					__m512i vSrcLow = _mm512_and_si512(vSrc, maskF);
					// dst & 0x0F
					__m512i vDstLow = _mm512_and_si512(vDst, maskF);

					// (srcLow + dstLow)
					__m512i vSum = _mm512_add_epi8(vSrcLow, vDstLow);

					// sum >> 1  (Note: srli works on 16/32/64, but since we masked 0xF, sum is max 0x1E, 
					// so no carry spill across byte boundaries. Safe to use wider shift or specific byte shift if avail.
					// _mm512_srli_epi16 is safe here because bits 4-7 are zero in vSum)
					__m512i vAvg = _mm512_srli_epi16(vSum, 1);
					// Ensure we clear any high bits if shift misbehaved (it won't with 0x1E max)
					vAvg = _mm512_and_si512(vAvg, maskF);

					// src & 0xF0
					__m512i vSrcHigh = _mm512_and_si512(vSrc, maskF0);

					// Final OR
					__m512i vResult = _mm512_or_si512(vAvg, vSrcHigh);

					_mm512_mask_storeu_epi8(dst, mask, vResult);

					src += 64; dst += 64; n -= 64;
				}

				pixels += count;
				data += count - 1;
			}
			else {
				// Fallback
				while (count--) {
					if (pixels >= pixels_ll && pixels < pixels_ul)
						*pixels = (((*data & 0x0f) + (*pixels & 0x0f)) / 2) | (*data & 0xf0);
					pixels++; data++;
				}
				data--;
			}
		}
	}
}

static void blit_sprite2_blend_generic(Uint8* pixels, const Uint8* data,
	const Uint8* pixels_ll, const Uint8* pixels_ul,
	int pitch)
{
	for (; *data != 0x0f; ++data)
	{
		pixels += *data & 0x0f;
		unsigned int count = (*data & 0xf0) >> 4;

		if (count == 0) pixels += pitch - 12;
		else
		{
			data++;
			// Unrolled loop optimization
			while (count >= 4) {
				if ((pixels + 3) < pixels_ul && pixels >= pixels_ll) {
					pixels[0] = (((data[0] & 0x0f) + (pixels[0] & 0x0f)) >> 1) | (data[0] & 0xf0);
					pixels[1] = (((data[1] & 0x0f) + (pixels[1] & 0x0f)) >> 1) | (data[1] & 0xf0);
					pixels[2] = (((data[2] & 0x0f) + (pixels[2] & 0x0f)) >> 1) | (data[2] & 0xf0);
					pixels[3] = (((data[3] & 0x0f) + (pixels[3] & 0x0f)) >> 1) | (data[3] & 0xf0);
				}
				pixels += 4; data += 4; count -= 4;
			}
			while (count--) {
				if (pixels >= pixels_ul) return;
				if (pixels >= pixels_ll)
					*pixels = (((*data & 0x0f) + (*pixels & 0x0f)) / 2) | (*data & 0xf0);
				pixels++; data++;
			}
			data--;
		}
	}
}

void blit_sprite2_blend(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	assert(surface->format->BitsPerPixel == 8);
	Uint8* pixels = (Uint8*)surface->pixels + (y * surface->pitch) + x;
	const Uint8* pixels_ll = (Uint8*)surface->pixels;
	const Uint8* pixels_ul = (Uint8*)surface->pixels + (surface->h * surface->pitch);
	const Uint8* data = sprite2s.data + SDL_SwapLE16(((Uint16*)sprite2s.data)[index - 1]);

	if (g_hasAVX512 == 1) {
		blit_sprite2_blend_AVX512(pixels, data, pixels_ll, pixels_ul, VGAScreen->pitch);
	}
	else {
		blit_sprite2_blend_generic(pixels, data, pixels_ll, pixels_ul, VGAScreen->pitch);
	}
}

// ... [Remaining Standard Sprite2 Functions] ...

// does not clip on left or right edges of surface
void blit_sprite2_darken(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	assert(surface->format->BitsPerPixel == 8);
	Uint8* pixels = (Uint8*)surface->pixels + (y * surface->pitch) + x;
	const Uint8* const pixels_ll = (Uint8*)surface->pixels,  // lower limit
		* const pixels_ul = (Uint8*)surface->pixels + (surface->h * surface->pitch);  // upper limit

	const Uint8* data = sprite2s.data + SDL_SwapLE16(((Uint16*)sprite2s.data)[index - 1]);

	for (; *data != 0x0f; ++data)
	{
		pixels += *data & 0x0f;                   // second nibble: transparent pixel count
		unsigned int count = (*data & 0xf0) >> 4; // first nibble: opaque pixel count

		if (count == 0) // move to next pixel row
		{
			pixels += VGAScreen->pitch - 12;
		}
		else
		{
			while (count--)
			{
				++data;

				if (pixels >= pixels_ul)
					return;
				if (pixels >= pixels_ll)
					*pixels = ((*pixels & 0x0f) / 2) + (*pixels & 0xf0);

				++pixels;
			}
		}
	}
}

// does not clip on left or right edges of surface
void blit_sprite2_filter(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index, Uint8 filter)
{
	assert(surface->format->BitsPerPixel == 8);
	Uint8* pixels = (Uint8*)surface->pixels + (y * surface->pitch) + x;
	const Uint8* const pixels_ll = (Uint8*)surface->pixels,  // lower limit
		* const pixels_ul = (Uint8*)surface->pixels + (surface->h * surface->pitch);  // upper limit

	const Uint8* data = sprite2s.data + SDL_SwapLE16(((Uint16*)sprite2s.data)[index - 1]);

	for (; *data != 0x0f; ++data)
	{
		pixels += *data & 0x0f;                   // second nibble: transparent pixel count
		unsigned int count = (*data & 0xf0) >> 4; // first nibble: opaque pixel count

		if (count == 0) // move to next pixel row
		{
			pixels += VGAScreen->pitch - 12;
		}
		else
		{
			while (count--)
			{
				++data;

				if (pixels >= pixels_ul)
					return;
				if (pixels >= pixels_ll)
					*pixels = filter | (*data & 0x0f);

				++pixels;
			}
		}
	}
}

void blit_sprite2_filter_clip(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index, Uint8 filter)
{
	assert(surface->format->BitsPerPixel == 8);

	const Uint8* data = sprite2s.data + SDL_SwapLE16(((Uint16*)sprite2s.data)[index - 1]);

	for (; *data != 0x0f; ++data)
	{
		if (y >= surface->h)
			return;

		Uint8 skip_count = *data & 0x0f;
		Uint8 fill_count = (*data >> 4) & 0x0f;

		x += skip_count;

		if (fill_count == 0) // move to next pixel row
		{
			y += 1;
			x -= 12;
		}
		else if (y >= 0)
		{
			Uint8* const pixel_row = (Uint8*)surface->pixels + (y * surface->pitch);
			do
			{
				++data;

				if (x >= 0 && x < surface->pitch)
					pixel_row[x] = filter | (*data & 0x0f);;
				x += 1;
			} while (--fill_count);
		}
		else
		{
			data += fill_count;
			x += fill_count;
		}
	}
}

// does not clip on left or right edges of surface
void blit_sprite2x2(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	blit_sprite2(surface, x, y, sprite2s, index);
	blit_sprite2(surface, x + 12, y, sprite2s, index + 1);
	blit_sprite2(surface, x, y + 14, sprite2s, index + 19);
	blit_sprite2(surface, x + 12, y + 14, sprite2s, index + 20);
}

void blit_sprite2x2_clip(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	blit_sprite2_clip(surface, x, y, sprite2s, index);
	blit_sprite2_clip(surface, x + 12, y, sprite2s, index + 1);
	blit_sprite2_clip(surface, x, y + 14, sprite2s, index + 19);
	blit_sprite2_clip(surface, x + 12, y + 14, sprite2s, index + 20);
}

// does not clip on left or right edges of surface
void blit_sprite2x2_blend(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	blit_sprite2_blend(surface, x, y, sprite2s, index);
	blit_sprite2_blend(surface, x + 12, y, sprite2s, index + 1);
	blit_sprite2_blend(surface, x, y + 14, sprite2s, index + 19);
	blit_sprite2_blend(surface, x + 12, y + 14, sprite2s, index + 20);
}

// does not clip on left or right edges of surface
void blit_sprite2x2_darken(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index)
{
	blit_sprite2_darken(surface, x, y, sprite2s, index);
	blit_sprite2_darken(surface, x + 12, y, sprite2s, index + 1);
	blit_sprite2_darken(surface, x, y + 14, sprite2s, index + 19);
	blit_sprite2_darken(surface, x + 12, y + 14, sprite2s, index + 20);
}

// does not clip on left or right edges of surface
void blit_sprite2x2_filter(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index, Uint8 filter)
{
	blit_sprite2_filter(surface, x, y, sprite2s, index, filter);
	blit_sprite2_filter(surface, x + 12, y, sprite2s, index + 1, filter);
	blit_sprite2_filter(surface, x, y + 14, sprite2s, index + 19, filter);
	blit_sprite2_filter(surface, x + 12, y + 14, sprite2s, index + 20, filter);
}

void blit_sprite2x2_filter_clip(SDL_Surface* surface, int x, int y, Sprite2_array sprite2s, unsigned int index, Uint8 filter)
{
	blit_sprite2_filter_clip(surface, x, y, sprite2s, index, filter);
	blit_sprite2_filter_clip(surface, x + 12, y, sprite2s, index + 1, filter);
	blit_sprite2_filter_clip(surface, x, y + 14, sprite2s, index + 19, filter);
	blit_sprite2_filter_clip(surface, x + 12, y + 14, sprite2s, index + 20, filter);
}

void JE_loadMainShapeTables(const char* shpfile)
{
	enum { SHP_NUM = 13 };

	FILE* f = dir_fopen_die(data_dir(), shpfile, "rb");

	JE_word shpNumb;
	JE_longint shpPos[SHP_NUM + 1]; // +1 for storing file length

	fread_u16_die(&shpNumb, 1, f);
	assert(shpNumb + 1u == COUNTOF(shpPos));

	fread_s32_die(shpPos, shpNumb, f);

	fseek(f, 0, SEEK_END);
	for (unsigned int i = shpNumb; i < COUNTOF(shpPos); ++i)
		shpPos[i] = ftell(f);

	int i;
	// fonts, interface, option sprites
	for (i = 0; i < 7; i++)
	{
		fseek(f, shpPos[i], SEEK_SET);
		load_sprites(i, f);
	}

	// player shot sprites
	spriteSheet8.size = shpPos[i + 1] - shpPos[i];
	JE_loadCompShapesB(&spriteSheet8, f);
	i++;

	// player ship sprites
	spriteSheet9.size = shpPos[i + 1] - shpPos[i];
	JE_loadCompShapesB(&spriteSheet9, f);
	i++;

	// power-up sprites
	spriteSheet10.size = shpPos[i + 1] - shpPos[i];
	JE_loadCompShapesB(&spriteSheet10, f);
	i++;

	// coins, datacubes, etc sprites
	spriteSheet11.size = shpPos[i + 1] - shpPos[i];
	JE_loadCompShapesB(&spriteSheet11, f);
	i++;

	// more player shot sprites
	spriteSheet12.size = shpPos[i + 1] - shpPos[i];
	JE_loadCompShapesB(&spriteSheet12, f);
	i++;

	// tyrian 2000 ship sprites
	spriteSheetT2000.size = shpPos[i + 1] - shpPos[i];
	JE_loadCompShapesB(&spriteSheetT2000, f);

	fclose(f);
}

void free_main_shape_tables(void)
{
	for (uint i = 0; i < COUNTOF(sprite_table); ++i)
		free_sprites(i);

	free_sprite2s(&spriteSheet8);
	free_sprite2s(&spriteSheet9);
	free_sprite2s(&spriteSheet10);
	free_sprite2s(&spriteSheet11);
	free_sprite2s(&spriteSheet12);
}
