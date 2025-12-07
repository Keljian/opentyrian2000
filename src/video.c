/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) 2007-2009  The OpenTyrian Development Team
 * * GPU Backend: AVX-512 / AVX2 / Scalar Path Selector
 */

#include "video.h"

#include "keyboard.h"
#include "opentyr.h"
#include "palette.h"
#include "simd_detect.h"
#include "video_scale.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

 // Required for AVX/AVX512 intrinsics
#include <immintrin.h>

#include "SDL_opengl.h"
#include "SDL_opengl_glext.h" 

#ifdef _WIN32
#include <windows.h>
#endif

// --- COMPILER MACROS ---------------------------------------------------------
#if defined(_MSC_VER)
#define FORCE_VECTORIZATION __pragma(loop(ivdep))
#elif defined(__GNUC__) || defined(__clang__)
#define FORCE_VECTORIZATION _Pragma("GCC ivdep")
#else
#define FORCE_VECTORIZATION
#endif

// --- GL EXTENSIONS -----------------------------------------------------------
#define GL_FUNC_LIST \
    GL_FUNC(PFNGLCREATESHADERPROC, glCreateShader) \
    GL_FUNC(PFNGLSHADERSOURCEPROC, glShaderSource) \
    GL_FUNC(PFNGLCOMPILESHADERPROC, glCompileShader) \
    GL_FUNC(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    GL_FUNC(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    GL_FUNC(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    GL_FUNC(PFNGLATTACHSHADERPROC, glAttachShader) \
    GL_FUNC(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    GL_FUNC(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    GL_FUNC(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    GL_FUNC(PFNGLDELETESHADERPROC, glDeleteShader) \
    GL_FUNC(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    GL_FUNC(PFNGLUSEPROGRAMPROC, glUseProgram) \
    GL_FUNC(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    GL_FUNC(PFNGLUNIFORM2FPROC, glUniform2f) \
    GL_FUNC(PFNGLUNIFORM1IPROC, glUniform1i)

#define GL_FUNC(type, name) static type p_##name = NULL;
GL_FUNC_LIST
#undef GL_FUNC

#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glDeleteShader p_glDeleteShader
#define glDeleteProgram p_glDeleteProgram
#define glUseProgram p_glUseProgram
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform2f p_glUniform2f
#define glUniform1i p_glUniform1i

static void load_gl_extensions(void) {
#define GL_FUNC(type, name) p_##name = (type)SDL_GL_GetProcAddress(#name);
    GL_FUNC_LIST
#undef GL_FUNC
}

// --- GAMMA SAFETY ------------------------------------------------------------
static SDL_Window* global_window_ref = NULL;

void force_normal_gamma(void) {
#ifdef _WIN32
    HDC hDC = GetDC(NULL);
    if (hDC) {
        WORD ramp[3][256];
        for (int i = 0; i < 256; i++) {
            WORD val = (WORD)(i * 257);
            ramp[0][i] = val; ramp[1][i] = val; ramp[2][i] = val;
        }
        SetDeviceGammaRamp(hDC, ramp);
        ReleaseDC(NULL, hDC);
    }
#else
    if (global_window_ref) {
        Uint16 ramp[256];
        for (int i = 0; i < 256; ++i) ramp[i] = (Uint16)(i * 257);
        SDL_SetWindowGammaRamp(global_window_ref, ramp, ramp, ramp);
    }
#endif
}

// --- GLOBALS -----------------------------------------------------------------
const char* const scaling_mode_names[ScalingMode_MAX] = {
    "Center", "Integer", "Fit 8:5", "Fit 4:3",
};

int fullscreen_display;
ScalingMode scaling_mode = SCALE_ASPECT_4_3;
static SDL_Rect last_output_rect = { 0, 0, vga_width, vga_height };

SDL_Surface* VGAScreen = NULL, * VGAScreenSeg = NULL;
SDL_Surface* VGAScreen2 = NULL;
SDL_Surface* game_screen = NULL;

SDL_Window* main_window = NULL;
SDL_GLContext gl_context = NULL;
SDL_PixelFormat* main_window_tex_format = NULL;

static GLuint texture_id = 0;
static GLuint program_id = 0;
static Uint32* rgb_buffer = NULL;

// --- SHADERS -----------------------------------------------------------------
static const char* vertex_shader_src =
"#version 120\n"
"varying vec2 TexCoord;\n"
"void main() { gl_Position = gl_Vertex; TexCoord = gl_MultiTexCoord0.xy; }";

static const char* fragment_shader_bilateral_src =
"#version 120\n"
"uniform sampler2D gameTexture;\n"
"varying vec2 TexCoord;\n"
"const vec2 texSize = vec2(320.0, 200.0);\n"
"void main() {\n"
"    vec2 texel = 1.0 / texSize;\n"
"    vec3 C = texture2D(gameTexture, TexCoord).rgb;\n"
"    vec3 sum = C;\n"
"    float w_sum = 1.0;\n"
"    vec2 offsets[4];\n"
"    offsets[0] = vec2(-texel.x, 0.0); offsets[1] = vec2( texel.x, 0.0);\n"
"    offsets[2] = vec2( 0.0, -texel.y); offsets[3] = vec2( 0.0,  texel.y);\n"
"    float sigma = 0.15;\n"
"    for(int i=0; i<4; i++) {\n"
"        vec3 samp = texture2D(gameTexture, TexCoord + offsets[i]).rgb;\n"
"        float dist = distance(C, samp);\n"
"        float w = exp(-(dist * dist) / (2.0 * sigma * sigma));\n"
"        sum += samp * w; w_sum += w;\n"
"    }\n"
"    gl_FragColor = vec4(sum / w_sum, 1.0);\n"
"}";

// --- RENDERER ----------------------------------------------------------------

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, NULL, log);
        fprintf(stderr, "Shader Compile Error: %s\n", log);
    }
    return shader;
}

static void init_gl_resources(void) {
    load_gl_extensions();
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_bilateral_src);
    program_id = glCreateProgram();
    glAttachShader(program_id, vs);
    glAttachShader(program_id, fs);
    glLinkProgram(program_id);
    glDeleteShader(vs); glDeleteShader(fs);
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vga_width, vga_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    rgb_buffer = (Uint32*)malloc(vga_width * vga_height * sizeof(Uint32));
}

static void calc_dst_render_rect(SDL_Rect* const dst_rect) {
    int win_w, win_h;
    SDL_GetWindowSize(main_window, &win_w, &win_h);
    dst_rect->w = win_w; dst_rect->h = win_h;
    int maxh_width, maxw_height;
    switch (scaling_mode) {
    case SCALE_CENTER: case SCALE_INTEGER: case SCALE_ASPECT_4_3:
        maxh_width = (int)(win_h * (4.0f / 3.0f));
        maxw_height = (int)(win_w * (3.0f / 4.0f));
        if (maxh_width > win_w) { dst_rect->w = win_w; dst_rect->h = maxw_height; }
        else { dst_rect->w = maxh_width; dst_rect->h = win_h; }
        break;
    case SCALE_ASPECT_8_5:
        maxh_width = (int)(win_h * (8.0f / 5.0f));
        maxw_height = (int)(win_w * (5.0f / 8.0f));
        if (maxh_width > win_w) { dst_rect->w = win_w; dst_rect->h = maxw_height; }
        else { dst_rect->w = maxh_width; dst_rect->h = win_h; }
        break;
    default: break;
    }
    dst_rect->x = (win_w - dst_rect->w) / 2;
    dst_rect->y = (win_h - dst_rect->h) / 2;
}

static void scale_and_flip(SDL_Surface* src_surface)
{
    const Uint8* __restrict src = (const Uint8*)src_surface->pixels;
    Uint32* __restrict dst = rgb_buffer;
    const Uint32* __restrict pal = rgb_palette;
    const int count = vga_width * vga_height;
    int i = 0;

    // --- AVX-512 PATH (OPTIMIZED: 64 Pixels per loop) ------------------------
    // Requires Compiler Flag: /arch:AVX512 or -mavx512f
    // Checks runtime CPU support to prevent crashes on older hardware.
#if defined(__AVX512F__)
    if (SDL_HasAVX512F()) {
        // Unroll factor 4 (4 * 16 = 64 pixels per step)
        // 64000 is divisible by 64, so no boundary check needed for standard resolution
        for (; i <= count - 64; i += 64) {
            // Load 64 bytes (16 bytes * 4) of indices
            __m128i idx0 = _mm_loadu_si128((__m128i*) & src[i]);
            __m128i idx1 = _mm_loadu_si128((__m128i*) & src[i + 16]);
            __m128i idx2 = _mm_loadu_si128((__m128i*) & src[i + 32]);
            __m128i idx3 = _mm_loadu_si128((__m128i*) & src[i + 48]);

            // Expand to 512-bit registers (16x 32-bit integers each)
            __m512i v0 = _mm512_cvtepu8_epi32(idx0);
            __m512i v1 = _mm512_cvtepu8_epi32(idx1);
            __m512i v2 = _mm512_cvtepu8_epi32(idx2);
            __m512i v3 = _mm512_cvtepu8_epi32(idx3);

            // Gather 16 RGBA colors for each vector
            // Scale is 4 (sizeof Uint32)
            v0 = _mm512_i32gather_epi32(v0, pal, 4);
            v1 = _mm512_i32gather_epi32(v1, pal, 4);
            v2 = _mm512_i32gather_epi32(v2, pal, 4);
            v3 = _mm512_i32gather_epi32(v3, pal, 4);

            // Store results (64 bytes each = 256 bytes total written)
            _mm512_storeu_si512((void*)&dst[i], v0);
            _mm512_storeu_si512((void*)&dst[i + 16], v1);
            _mm512_storeu_si512((void*)&dst[i + 32], v2);
            _mm512_storeu_si512((void*)&dst[i + 48], v3);
        }
    }
#endif // End AVX512 block

    // --- AVX2 PATH (OPTIMIZED: 32 Pixels per loop) ---------------------------
    // Fallback if AVX512 is not present but AVX2 is.
    // Note: We use the 'i' counter from above, so if AVX512 ran, this is skipped.
#if defined(__AVX2__)
    if (i == 0 && SDL_HasAVX2()) {
        for (; i <= count - 32; i += 32) {
            // Load 32 bytes (8 bytes * 4)
            __m128i idx0_8 = _mm_loadl_epi64((__m128i*) & src[i]);
            __m128i idx1_8 = _mm_loadl_epi64((__m128i*) & src[i + 8]);
            __m128i idx2_8 = _mm_loadl_epi64((__m128i*) & src[i + 16]);
            __m128i idx3_8 = _mm_loadl_epi64((__m128i*) & src[i + 24]);

            // Expand to 256-bit
            __m256i v0 = _mm256_cvtepu8_epi32(idx0_8);
            __m256i v1 = _mm256_cvtepu8_epi32(idx1_8);
            __m256i v2 = _mm256_cvtepu8_epi32(idx2_8);
            __m256i v3 = _mm256_cvtepu8_epi32(idx3_8);

            // Gather
            v0 = _mm256_i32gather_epi32((const int*)pal, v0, 4);
            v1 = _mm256_i32gather_epi32((const int*)pal, v1, 4);
            v2 = _mm256_i32gather_epi32((const int*)pal, v2, 4);
            v3 = _mm256_i32gather_epi32((const int*)pal, v3, 4);

            // Store
            _mm256_storeu_si256((__m256i*) & dst[i], v0);
            _mm256_storeu_si256((__m256i*) & dst[i + 8], v1);
            _mm256_storeu_si256((__m256i*) & dst[i + 16], v2);
            _mm256_storeu_si256((__m256i*) & dst[i + 24], v3);
        }
    }
#endif

    // --- SCALAR FALLBACK -----------------------------------------------------
    // Handles cleanup or non-AVX systems.
    FORCE_VECTORIZATION
        for (; i < count; ++i) {
            dst[i] = pal[src[i]];
        }
    // -------------------------------------------------------------------------

    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vga_width, vga_height, GL_BGRA, GL_UNSIGNED_BYTE, rgb_buffer);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    SDL_Rect dst_rect;
    calc_dst_render_rect(&dst_rect);
    int win_w, win_h;
    SDL_GetWindowSize(main_window, &win_w, &win_h);
    glViewport(dst_rect.x, win_h - (dst_rect.y + dst_rect.h), dst_rect.w, dst_rect.h);

    glUseProgram(program_id);
    GLint loc_res = glGetUniformLocation(program_id, "resolution");
    if (loc_res != -1) glUniform2f(loc_res, (float)dst_rect.w, (float)dst_rect.h);
    GLint loc_tex = glGetUniformLocation(program_id, "gameTexture");
    if (loc_tex != -1) glUniform1i(loc_tex, 0);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, 1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, 1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, -1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
    glEnd();

    glUseProgram(0);
    SDL_GL_SwapWindow(main_window);
    last_output_rect = dst_rect;
}

// --- INIT --------------------------------------------------------------------

void init_video(void) {
    if (SDL_WasInit(SDL_INIT_VIDEO)) return;
    detect_cpu_features(); // Updates SDL SIMD flags
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) exit(1);

    VGAScreen = VGAScreenSeg = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
    VGAScreen2 = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
    game_screen = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
    JE_clr256(VGAScreen);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);

    main_window = SDL_CreateWindow(opentyrian_str, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        vga_width * 3, vga_height * 3, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!main_window) exit(1);
    global_window_ref = main_window;
    atexit(force_normal_gamma);

    gl_context = SDL_GL_CreateContext(main_window);
    SDL_GL_SetSwapInterval(1);
    init_gl_resources();
    main_window_tex_format = SDL_AllocFormat(SDL_PIXELFORMAT_ARGB8888);
    reinit_fullscreen(fullscreen_display);
    SDL_ShowWindow(main_window);
}

bool init_scaler(unsigned int new_scaler) { scaler = new_scaler; return true; }

int get_display_refresh_rate(void) {
    SDL_DisplayMode mode;
    int index = (main_window) ? SDL_GetWindowDisplayIndex(main_window) : 0;
    if (SDL_GetCurrentDisplayMode(index < 0 ? 0 : index, &mode) == 0 && mode.refresh_rate > 0)
        return mode.refresh_rate;
    return 60;
}

void deinit_video(void) {
    force_normal_gamma();
    if (rgb_buffer) free(rgb_buffer);
    if (texture_id) glDeleteTextures(1, &texture_id);
    if (program_id) glDeleteProgram(program_id);
    if (gl_context) SDL_GL_DeleteContext(gl_context);
    if (main_window) SDL_DestroyWindow(main_window);
    if (main_window_tex_format) SDL_FreeFormat(main_window_tex_format);
    SDL_FreeSurface(VGAScreenSeg); SDL_FreeSurface(VGAScreen2); SDL_FreeSurface(game_screen);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void reinit_fullscreen(int new_display) {
    fullscreen_display = new_display;
    if (fullscreen_display >= SDL_GetNumVideoDisplays()) fullscreen_display = 0;
    SDL_SetWindowFullscreen(main_window, 0);
    if (fullscreen_display != -1) {
        // Center code inlined
        int w, h; SDL_GetWindowSize(main_window, &w, &h);
        SDL_Rect bounds;
        if (SDL_GetDisplayBounds(fullscreen_display, &bounds) == 0)
            SDL_SetWindowPosition(main_window, bounds.x + (bounds.w - w) / 2, bounds.y + (bounds.h - h) / 2);
        SDL_SetWindowFullscreen(main_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
}

void video_on_win_resize(void) {}
void toggle_fullscreen(void) {
    if (fullscreen_display != -1) reinit_fullscreen(-1);
    else reinit_fullscreen(SDL_GetWindowDisplayIndex(main_window));
}
bool set_scaling_mode_by_name(const char* name) {
    for (int i = 0; i < ScalingMode_MAX; ++i) if (strcmp(name, scaling_mode_names[i]) == 0) { scaling_mode = i; return true; }
    return false;
}
void JE_clr256(SDL_Surface* screen) { if (screen) SDL_FillRect(screen, NULL, 0); }
void JE_showVGA(void) { if (VGAScreen) scale_and_flip(VGAScreen); }

void mapScreenPointToWindow(Sint32* x, Sint32* y) {
    if (!VGAScreen || last_output_rect.w == 0) return;
    float sx = (float)last_output_rect.w / VGAScreen->w, sy = (float)last_output_rect.h / VGAScreen->h;
    *x = (Sint32)(*x * sx) + last_output_rect.x; *y = (Sint32)(*y * sy) + last_output_rect.y;
}
void mapWindowPointToScreen(Sint32* x, Sint32* y) {
    if (!VGAScreen || last_output_rect.w == 0) return;
    *x = (Sint32)((float)(*x - last_output_rect.x) * (VGAScreen->w / (float)last_output_rect.w));
    *y = (Sint32)((float)(*y - last_output_rect.y) * (VGAScreen->h / (float)last_output_rect.h));
}
void scaleWindowDistanceToScreen(Sint32* x, Sint32* y) {
    if (!VGAScreen || last_output_rect.w == 0) return;
    *x = (Sint32)(*x * ((float)VGAScreen->w / last_output_rect.w));
    *y = (Sint32)(*y * ((float)VGAScreen->h / last_output_rect.h));
}
