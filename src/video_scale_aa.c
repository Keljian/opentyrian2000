/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Advanced Anti-Aliasing for pixel art upscaling
 *
 * Implements FXAA-inspired techniques optimized for retro game graphics
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
#include <omp.h>

// Helper macros
#define RED(c)   (((c) >> 16) & 0xFF)
#define GREEN(c) (((c) >> 8) & 0xFF)
#define BLUE(c)  ((c) & 0xFF)
#define MAKE_RGB(r, g, b) (((r) << 16) | ((g) << 8) | (b))
#define CLAMP(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))

// Calculate luminance using perceptual weights
static inline float luminance(Uint32 color)
{
	return (RED(color) * 0.299f + GREEN(color) * 0.587f + BLUE(color) * 0.114f) / 255.0f;
}

// Linear interpolation between two colors
static inline Uint32 lerp_color(Uint32 a, Uint32 b, float t)
{
	int r = (int)(RED(a) * (1.0f - t) + RED(b) * t);
	int g = (int)(GREEN(a) * (1.0f - t) + GREEN(b) * t);
	int bl = (int)(BLUE(a) * (1.0f - t) + BLUE(b) * t);
	return MAKE_RGB(CLAMP(r), CLAMP(g), CLAMP(bl));
}

/*
 * FXAA-inspired edge detection and anti-aliasing
 * Detects edges based on luminance contrast and applies directional smoothing
 */
static Uint32 fxaa_pixel(Uint32 center, Uint32 N, Uint32 S, Uint32 E, Uint32 W,
                          Uint32 NW, Uint32 NE, Uint32 SW, Uint32 SE)
{
	// Calculate luminances
	float lum_C = luminance(center);
	float lum_N = luminance(N);
	float lum_S = luminance(S);
	float lum_E = luminance(E);
	float lum_W = luminance(W);
	float lum_NW = luminance(NW);
	float lum_NE = luminance(NE);
	float lum_SW = luminance(SW);
	float lum_SE = luminance(SE);

	// Calculate min/max luminance in neighborhood
	float lum_min = lum_C;
	float lum_max = lum_C;

	lum_min = fminf(lum_min, fminf(fminf(lum_N, lum_S), fminf(lum_E, lum_W)));
	lum_max = fmaxf(lum_max, fmaxf(fmaxf(lum_N, lum_S), fmaxf(lum_E, lum_W)));

	float contrast = lum_max - lum_min;

	// If contrast is low, no AA needed
	if (contrast < 0.05f)
	{
		return center;
	}

	// Calculate edge direction
	float edge_horz = fabsf((lum_N + lum_S) - 2.0f * lum_C);
	float edge_vert = fabsf((lum_E + lum_W) - 2.0f * lum_C);

	// Detect diagonal edges
	float edge_diag1 = fabsf((lum_NW + lum_SE) - 2.0f * lum_C); // NW-SE diagonal
	float edge_diag2 = fabsf((lum_NE + lum_SW) - 2.0f * lum_C); // NE-SW diagonal

	// Choose blending direction based on strongest edge
	Uint32 result = center;

	if (edge_horz > edge_vert * 1.2f)
	{
		// Horizontal edge - blend vertically
		float blend = fminf(contrast * 0.5f, 0.5f);
		Uint32 blend_N = lerp_color(center, N, blend);
		Uint32 blend_S = lerp_color(center, S, blend);
		result = lerp_color(blend_N, blend_S, 0.5f);
	}
	else if (edge_vert > edge_horz * 1.2f)
	{
		// Vertical edge - blend horizontally
		float blend = fminf(contrast * 0.5f, 0.5f);
		Uint32 blend_E = lerp_color(center, E, blend);
		Uint32 blend_W = lerp_color(center, W, blend);
		result = lerp_color(blend_E, blend_W, 0.5f);
	}
	else if (edge_diag1 > fmaxf(edge_horz, edge_vert))
	{
		// NW-SE diagonal
		float blend = fminf(contrast * 0.4f, 0.4f);
		Uint32 blend_NW = lerp_color(center, NW, blend);
		Uint32 blend_SE = lerp_color(center, SE, blend);
		result = lerp_color(blend_NW, blend_SE, 0.5f);
	}
	else if (edge_diag2 > fmaxf(edge_horz, edge_vert))
	{
		// NE-SW diagonal
		float blend = fminf(contrast * 0.4f, 0.4f);
		Uint32 blend_NE = lerp_color(center, NE, blend);
		Uint32 blend_SW = lerp_color(center, SW, blend);
		result = lerp_color(blend_NE, blend_SW, 0.5f);
	}
	else
	{
		// No clear direction - use subtle 4-way blend
		float blend = fminf(contrast * 0.25f, 0.25f);
		int r = RED(center);
		int g = GREEN(center);
		int b = BLUE(center);

		r += (int)((RED(N) + RED(S) + RED(E) + RED(W) - 4 * r) * blend);
		g += (int)((GREEN(N) + GREEN(S) + GREEN(E) + GREEN(W) - 4 * g) * blend);
		b += (int)((BLUE(N) + BLUE(S) + BLUE(E) + BLUE(W) - 4 * b) * blend);

		result = MAKE_RGB(CLAMP(r), CLAMP(g), CLAMP(b));
	}

	return result;
}


static Uint32* fxaa_buffer = NULL;
static size_t fxaa_buffer_capacity = 0;
/*
 * Apply FXAA-style anti-aliasing to scaled output
 * This is a post-process filter applied after scaling
 */
static void apply_fxaa(Uint32* pixels, int width, int height, int pitch_pixels)
{
	size_t required_size = width * height * sizeof(Uint32);

	// Only allocate if the buffer is uninitialized or too small (e.g. resolution changed)
	if (fxaa_buffer == NULL || required_size > fxaa_buffer_capacity)
	{
		Uint32* new_buffer = realloc(fxaa_buffer, required_size);

		if (!new_buffer)
		{
			// If allocation fails, we can't do AA, so just return.
			// Ideally, log an error here.
			return;
		}

		fxaa_buffer = new_buffer;
		fxaa_buffer_capacity = required_size;
	}

	// Copy input to our persistent buffer
	for (int y = 0; y < height; y++)
	{
		memcpy(fxaa_buffer + y * width, pixels + y * pitch_pixels, width * sizeof(Uint32));
	}
#pragma omp parallel 
	// Apply FXAA (Read from buffer, Write to pixels)
	for (int y = 1; y < height - 1; y++)
	{
		for (int x = 1; x < width - 1; x++)
		{
			// Current pixel index in the linear buffer
			int idx = y * width + x;

			// Sample neighbors from the persistent buffer
			Uint32 C = fxaa_buffer[idx];
			Uint32 N = fxaa_buffer[(y - 1) * width + x];
			Uint32 S = fxaa_buffer[(y + 1) * width + x];
			Uint32 E = fxaa_buffer[y * width + (x + 1)];
			Uint32 W = fxaa_buffer[y * width + (x - 1)];
			Uint32 NW = fxaa_buffer[(y - 1) * width + (x - 1)];
			Uint32 NE = fxaa_buffer[(y - 1) * width + (x + 1)];
			Uint32 SW = fxaa_buffer[(y + 1) * width + (x - 1)];
			Uint32 SE = fxaa_buffer[(y + 1) * width + (x + 1)];

			// Write processed pixel back to the texture memory
			pixels[y * pitch_pixels + x] = fxaa_pixel(C, N, S, E, W, NW, NE, SW, SE);
		}
	}

	// DO NOT free(fxaa_buffer) here. We keep it for the next frame.
}

/*
 * High-quality anti-aliased scaler
 * Combines upscaling with FXAA post-processing
 */
static void hires_scale_aa(SDL_Surface *src_surface, SDL_Texture *dst_texture, int scale)
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

	int dst_width = width * scale;
	int dst_height = height * scale;
	int pitch_pixels = dst_pitch / dst_Bpp;

	// First pass: Simple nearest neighbor upscaling
	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			Uint32 color = rgb_palette[*(src + y * src_pitch + x)];

			// Write scaled block
			for (int dy = 0; dy < scale; dy++)
			{
				for (int dx = 0; dx < scale; dx++)
				{
					int dst_x = x * scale + dx;
					int dst_y = y * scale + dy;
					*((Uint32*)(dst + dst_y * dst_pitch + dst_x * dst_Bpp)) = color;
				}
			}
		}
	}

	// Second pass: Apply FXAA anti-aliasing
	apply_fxaa((Uint32*)dst, dst_width, dst_height, pitch_pixels);

	SDL_UnlockTexture(dst_texture);
}

// Anti-aliased scalers for different resolutions
void aa5x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_aa(src_surface, dst_texture, 5);
}

void aa6x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_aa(src_surface, dst_texture, 6);
}

void aa7x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_aa(src_surface, dst_texture, 7);
}

void aa8x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_aa(src_surface, dst_texture, 8);
}

void aa10x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_aa(src_surface, dst_texture, 10);
}

void aa12x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_aa(src_surface, dst_texture, 12);
}

// Anti-aliased version for 3x and 4x
void aa3x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_aa(src_surface, dst_texture, 3);
}

void aa4x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture)
{
	hires_scale_aa(src_surface, dst_texture, 4);
}
