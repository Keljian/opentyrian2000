/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * High-resolution scaling optimized for 1440p and 4K displays
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "video_scale.h"
#include "palette.h"
#include "video.h"
#include "simd_detect.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Helper macros
#define RED(c)   (((c) >> 16) & 0xFF)
#define GREEN(c) (((c) >> 8) & 0xFF)
#define BLUE(c)  ((c) & 0xFF)
#define MAKE_RGB(r, g, b) (((r) << 16) | ((g) << 8) | (b))

#define CLAMP(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))

/*
 * Display Resolution Analysis:
 *
 * Original: 320x200 (16:10 aspect ratio)
 *
 * 1080p (1920x1080):
 *   - Best fit: 6x = 1920x1200 (crop 120px) or 5x = 1600x1000
 *   - Recommendation: 5x with Smooth5x or 6x centered
 *
 * 1440p (2560x1440):
 *   - Best fit: 7x = 2240x1400 (perfect fit!)
 *   - Recommendation: 7x with Smooth7x
 *
 * 4K (3840x2160):
 *   - Best fit: 10x = 3200x2000 or 12x = 3840x2400 (crop 240px)
 *   - Recommendation: 10x with Smooth10x or xBR-10x
 */

// Multi-pass bilateral filter for high-resolution scaling
static Uint32 enhanced_bilateral(Uint32 center, Uint32 neighbors[8], int strength)
{
	int r_sum = RED(center) * strength;
	int g_sum = GREEN(center) * strength;
	int b_sum = BLUE(center) * strength;
	int weight_sum = strength;

	// Calculate center luminance
	int center_lum = (RED(center) * 299 + GREEN(center) * 587 + BLUE(center) * 114) / 1000;

	// Edge threshold scales with smoothing strength
	int edge_threshold = 20 + (strength * 2);

	for (int i = 0; i < 8; i++)
	{
		int n_lum = (RED(neighbors[i]) * 299 + GREEN(neighbors[i]) * 587 + BLUE(neighbors[i]) * 114) / 1000;
		int lum_diff = abs(n_lum - center_lum);

		// Adaptive weighting based on edge strength
		if (lum_diff < edge_threshold)
		{
			int weight = (i < 4) ? 3 : 2; // Cardinal directions get more weight

			// Reduce weight for pixels near edges
			if (lum_diff > edge_threshold / 2)
			{
				weight = weight / 2;
			}

			r_sum += RED(neighbors[i]) * weight;
			g_sum += GREEN(neighbors[i]) * weight;
			b_sum += BLUE(neighbors[i]) * weight;
			weight_sum += weight;
		}
	}

	return MAKE_RGB(r_sum / weight_sum, g_sum / weight_sum, b_sum / weight_sum);
}

// Generic high-quality scaler with smoothing
static void hires_scale_smooth(SDL_Surface *src_surface, SDL_Texture *dst_texture, int scale, int smooth_strength)
{
	Uint8 *src = src_surface->pixels;
	Uint8 *dst;
	int src_pitch = src_surface->pitch;
	int dst_pitch;

	const int dst_Bpp = 4;
	const int height = vga_height;
	const int width = vga_width;

	void* tmp_ptr;
	SDL_LockTexture(dst_texture, NULL, &tmp_ptr, &dst_pitch);
	dst = tmp_ptr;

	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			Uint8 *s = src + y * src_pitch + x;
			Uint32 E = rgb_palette[*s];

			// Get 3x3 neighborhood for smoothing
			Uint32 neighbors[8];
			int idx = 0;

			for (int dy = -1; dy <= 1; dy++)
			{
				for (int dx = -1; dx <= 1; dx++)
				{
					if (dx == 0 && dy == 0) continue;

					int nx = x + dx;
					int ny = y + dy;

					// Clamp to bounds
					if (nx < 0) nx = 0;
					if (nx >= width) nx = width - 1;
					if (ny < 0) ny = 0;
					if (ny >= height) ny = height - 1;

					neighbors[idx++] = rgb_palette[*(src + ny * src_pitch + nx)];
				}
			}

			// Apply enhanced bilateral filtering
			Uint32 smoothed = enhanced_bilateral(E, neighbors, smooth_strength);

			// Write scaled block
			Uint8 *dst_pos = dst + (y * scale * dst_pitch) + (x * scale * dst_Bpp);
			for (int dy = 0; dy < scale; dy++)
			{
				for (int dx = 0; dx < scale; dx++)
				{
					*(Uint32 *)(dst_pos + dy * dst_pitch + dx * dst_Bpp) = smoothed;
				}
			}
		}
	}

	SDL_UnlockTexture(dst_texture);
}

// 5x scaler - optimal for 1080p displays (1600x1000)
void smooth5x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_smooth(src_surface, dst_texture, 5, 3);
}

// 6x scaler - fits 1080p with slight crop (1920x1200)
void smooth6x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_smooth(src_surface, dst_texture, 6, 4);
}

// 7x scaler - optimal for 1440p displays (2240x1400)
void smooth7x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_smooth(src_surface, dst_texture, 7, 4);
}

// 8x scaler - fills 1440p with crop (2560x1600)
void smooth8x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_smooth(src_surface, dst_texture, 8, 5);
}

// 10x scaler - optimal for 4K displays (3200x2000)
void smooth10x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_smooth(src_surface, dst_texture, 10, 6);
}

// 12x scaler - fills 4K with crop (3840x2400)
void smooth12x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_smooth(src_surface, dst_texture, 12, 7);
}

// Simple nearest neighbor for high resolutions (performance option)
static void nn_hires(SDL_Surface *src_surface, SDL_Texture *dst_texture, int scale)
{
	Uint8 *src = src_surface->pixels, *src_temp;
	Uint8 *dst, *dst_temp;

	int src_pitch = src_surface->pitch;
	int dst_pitch;

	const int dst_Bpp = 4;
	int dst_width, dst_height;
	SDL_QueryTexture(dst_texture, NULL, NULL, &dst_width, &dst_height);

	const int height = vga_height;
	const int width = vga_width;

	void* tmp_ptr;
	SDL_LockTexture(dst_texture, NULL, &tmp_ptr, &dst_pitch);
	dst = tmp_ptr;

	for (int y = height; y > 0; y--)
	{
		src_temp = src;
		dst_temp = dst;

		for (int x = width; x > 0; x--)
		{
			Uint32 color = rgb_palette[*src];
			for (int z = scale; z > 0; z--)
			{
				*(Uint32 *)dst = color;
				dst += dst_Bpp;
			}
			src++;
		}

		src = src_temp + src_pitch;
		dst = dst_temp + dst_pitch;

		for (int z = scale; z > 1; z--)
		{
			memcpy(dst, dst_temp, dst_width * dst_Bpp);
			dst += dst_pitch;
		}
	}

	SDL_UnlockTexture(dst_texture);
}

void nn5x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	nn_hires(src_surface, dst_texture, 5);
}

void nn6x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	nn_hires(src_surface, dst_texture, 6);
}

void nn7x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	nn_hires(src_surface, dst_texture, 7);
}

void nn8x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	nn_hires(src_surface, dst_texture, 8);
}

void nn10x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	nn_hires(src_surface, dst_texture, 10);
}

void nn12x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	nn_hires(src_surface, dst_texture, 12);
}
