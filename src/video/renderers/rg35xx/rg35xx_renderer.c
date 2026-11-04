/*
 * RG35XX renderer: the OpenGL3 renderer's paletted pipeline done on the CPU, presented through GLES2.
 *
 * The device's GPU (PowerVR SGX544) is GLES2 only: no integer textures, no 16-bit render targets, and every branch
 * or discard in a fragment shader costs it on every pixel. So the frame is composed here at 320x200, exactly as
 * shaders/palette.frag and shaders/rgba.frag compose it:
 *  - the paletted framebuffer keeps, per pixel, the four channels the GL3 renderer keeps in its RGBA16 target: the
 *    palette index (R), the remap code (G: table + 19 * rounds), the translucent index + 1 (B) and the additive
 *    index (A); the last three packed in one word (FX_*), kept apart and only for pixels whose index has the
 *    FX_FLAG bit (indexes need 10 bits): a plain pixel is one 16-bit store and load, the frame stays in the cache. Each sprite writes the channels its blend mode owns (MODE_SET all four, MODE_REMAP G, the shadow
 *    G by maximum, MODE_ADD A, MODE_TRANSLUCENT G, B and A), in the order the game draws, as the GL3 batches do.
 *    Like the GL3 target it is never cleared: every scene draws a full background;
 *  - the resolve (rgba.frag) turns the channels into a final palette index, then a 1024-entry RGB565 table into
 *    the 16-bit picture;
 *  - the picture goes into one of three textures in turn (the SGX copies a texture a frame in flight samples) and
 *    is drawn 2x, pixel perfect: 640x400 between black bars on the 640x480 panel. The swap waits for the vblank.
 */
#include "video/renderers/rg35xx/rg35xx_renderer.h"

#include "utils/allocator.h"
#include "utils/log.h"
#include "utils/miscmath.h"
#include "video/enums.h"
#include "video/vga_state.h"

#include <GLES2/gl2.h>
#include <SDL.h>
#include <math.h>
#include <string.h>

#define NATIVE_W 320
#define NATIVE_H 200
#define PIXELS (NATIVE_W * NATIVE_H)
#define SCREEN_W 640
#define SCREEN_H 480

// set in a pixel's index when its effect word counts (else the effects are all 0)
#define FX_FLAG 0x8000u
#define INDEX_MASK 0x3FFu

// the effect channels in one word: G bits 0-7, B bits 8-17, A bits 18-27
#define FX_G(v) ((uint32_t)(v))
#define FX_B(v) ((uint32_t)(v) << 8)
#define FX_A(v) ((uint32_t)(v) << 18)
#define FX_GET_G(fx) ((int)((fx) & 0xFF))
#define FX_GET_B(fx) ((int)(((fx) >> 8) & 0x3FF))
#define FX_GET_A(fx) ((int)(((fx) >> 18) & 0x3FF))

bool rg35xx_renderer_stats = false;

typedef enum
{
    MODE_SET,
    MODE_REMAP,
    MODE_ADD,
    MODE_TRANSLUCENT,
    MODE_SPRITE_SHADOW,
} blend_mode;

typedef struct rg35xx_context {
    SDL_Window *window;
    SDL_GLContext gl_context;
    GLuint program;
    GLuint textures[3];
    int next_texture;

    int screen_w, screen_h, aspect, fb_scale, scaling_mode;
    bool fullscreen, vsync;
    int move_x, move_y;
    unsigned framebuffer_options;

    // the paletted framebuffer (palette.frag's output channels)
    uint16_t index[PIXELS];
    uint32_t fx[PIXELS];

    // offscreen rendering (render_area_*): the area read back from the index channel
    irect area;

    // the palette and remap tables as of the last flush
    uint16_t rgb565[VGA_PALETTE_SIZE];
    uint8_t brightness[VGA_PALETTE_SIZE];
    vga_remap_tables remaps;

    // opacity decimation: palette.frag's noise() as a row term plus a column term, each in [0, 1)
    float row_noise[NATIVE_H];
    float column_noise[NATIVE_W];

    uint16_t picture[PIXELS];

    video_screenshot_signal screenshot_cb;

    // --stats
    double last_swap_ms, stats_since, work_sum, work_max, compose_sum;
    long stats_frames, stats_late, stats_sprites, stats_pixels, stats_mode_pixels[6];
} rg35xx_context;

static double now_ms(void) {
    return SDL_GetPerformanceCounter() * 1000.0 / SDL_GetPerformanceFrequency();
}

static bool is_available(void) {
    return true;
}

static const char *get_description(void) {
    return "Software renderer, GLES2 presentation (RG35XX)";
}

static const char *get_name(void) {
    return "RG35XX";
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if(!ok) {
        char text[512];
        glGetShaderInfoLog(s, sizeof text, NULL, text);
        log_error("RG35XX renderer: shader: %s", text);
    }
    return s;
}

// every buffer of the panel black: before the first picture, and on the way out (or an old picture flashes on the
// way back to MinUI: the panel scans out of more than one buffer)
static void clear_buffers(const rg35xx_context *ctx) {
    for(int i = 0; i < 3; i++) {
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(ctx->window);
    }
}

static bool setup_context(void *userdata, int window_w, int window_h, bool fullscreen, bool vsync, int aspect,
                          int framerate_limit, int fb_scale, int scaling_mode) {
    rg35xx_context *ctx = userdata;
    ctx->screen_w = window_w;
    ctx->screen_h = window_h;
    ctx->fullscreen = fullscreen;
    ctx->vsync = vsync;
    ctx->aspect = aspect;
    ctx->fb_scale = fb_scale;
    ctx->scaling_mode = scaling_mode;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    ctx->window = SDL_CreateWindow("OpenOMF", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_W, SCREEN_H,
                                   SDL_WINDOW_OPENGL);
    if(ctx->window == NULL || (ctx->gl_context = SDL_GL_CreateContext(ctx->window)) == NULL) {
        log_error("RG35XX renderer: no GL window: %s", SDL_GetError());
        return false;
    }
    // The game's loop is paced by the swap, which waits for the panel's vblank (whatever the vsync setting).
    SDL_GL_SetSwapInterval(1);
    SDL_ShowCursor(SDL_DISABLE);

    static const char *vs = "attribute vec2 pos; attribute vec2 uv; varying vec2 v;\n"
                            "void main() { v = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char *fs = "precision mediump float; varying vec2 v; uniform sampler2D t;\n"
                            "void main() { gl_FragColor = texture2D(t, v); }\n";
    ctx->program = glCreateProgram();
    glAttachShader(ctx->program, compile_shader(GL_VERTEX_SHADER, vs));
    glAttachShader(ctx->program, compile_shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(ctx->program, 0, "pos");
    glBindAttribLocation(ctx->program, 1, "uv");
    glLinkProgram(ctx->program);
    glUseProgram(ctx->program);
    glUniform1i(glGetUniformLocation(ctx->program, "t"), 0);

    glGenTextures(3, ctx->textures);
    for(int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, ctx->textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, NATIVE_W, NATIVE_H, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    }
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glClearColor(0, 0, 0, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    clear_buffers(ctx);

    // palette.frag's noise(): fract(tan(10 * PHI * y) + 0x6b * x / 256) at the fragment's centre, y counted from
    // the bottom of the 200-line target
    for(int y = 0; y < NATIVE_H; y++) {
        const double fy = (NATIVE_H - 1 - y) + 0.5;
        const double t = tan(10.0 * 1.61803398874989484820459 * fy);
        ctx->row_noise[y] = (float)(t - floor(t));
    }
    for(int x = 0; x < NATIVE_W; x++) {
        const double c = 0x6b * (x + 0.5) / 256.0;
        ctx->column_noise[x] = (float)(c - floor(c));
    }

    vga_remaps_init(&ctx->remaps);
    vga_state_mark_dirty();
    ctx->last_swap_ms = now_ms();
    log_info("RG35XX renderer initialized!");
    return true;
}

static void get_context_state(void *userdata, int *window_w, int *window_h, bool *fullscreen, bool *vsync, int *aspect,
                              int *fb_scale) {
    const rg35xx_context *ctx = userdata;
    if(window_w != NULL) {
        *window_w = ctx->screen_w;
    }
    if(window_h != NULL) {
        *window_h = ctx->screen_h;
    }
    if(fullscreen != NULL) {
        *fullscreen = ctx->fullscreen;
    }
    if(vsync != NULL) {
        *vsync = ctx->vsync;
    }
    if(aspect != NULL) {
        *aspect = ctx->aspect;
    }
    if(fb_scale != NULL) {
        *fb_scale = ctx->fb_scale;
    }
}

// The window is always the panel and the picture always 2x: the settings are only remembered.
static bool reset_context_with(void *userdata, int window_w, int window_h, bool fullscreen, bool vsync, int aspect,
                               int framerate_limit, int fb_scale, int scaling_mode) {
    rg35xx_context *ctx = userdata;
    ctx->screen_w = window_w;
    ctx->screen_h = window_h;
    ctx->fullscreen = fullscreen;
    ctx->vsync = vsync;
    ctx->aspect = aspect;
    ctx->fb_scale = fb_scale;
    ctx->scaling_mode = scaling_mode;
    return true;
}

static void reset_context(void *userdata) {
}

static void close_context(void *userdata) {
    rg35xx_context *ctx = userdata;
    clear_buffers(ctx);
    glFinish();
    glDeleteTextures(3, ctx->textures);
    glDeleteProgram(ctx->program);
    SDL_GL_DeleteContext(ctx->gl_context);
    SDL_DestroyWindow(ctx->window);
    log_info("RG35XX renderer closed.");
}

static inline blend_mode mode_of(unsigned int options, int remap_rounds) {
    if(options & SPRITE_TRANSLUCENT) {
        return MODE_TRANSLUCENT;
    }
    if(options & SPRITE_SHADOW) {
        return MODE_SPRITE_SHADOW;
    }
    if(remap_rounds > 0) {
        return MODE_REMAP;
    }
    if(options & SPRITE_INDEX_ADD) {
        return MODE_ADD;
    }
    return MODE_SET;
}

/*
 * One sprite into the paletted framebuffer: palette.frag for each pixel the quad covers, the texel picked as the
 * GPU's nearest filter picks it (the centre of the destination pixel, scaled and flipped into the source).
 */
static void draw_surface(void *userdata, const surface *src, irect *dst, int remap_offset, int remap_rounds,
                         int palette_offset, int palette_limit, int opacity, unsigned int flip_mode,
                         unsigned int options) {
    rg35xx_context *ctx = userdata;
    const int tw = src->w, th = src->h;
    if(dst->w <= 0 || dst->h <= 0 || tw <= 0 || th <= 0 || src->data == NULL) {
        return;
    }
    const int x0 = max2(dst->x, 0), x1 = min2(dst->x + dst->w, NATIVE_W);
    const int y0 = max2(dst->y, 0), y1 = min2(dst->y + dst->h, NATIVE_H);
    if(x0 >= x1 || y0 >= y1) {
        return;
    }

    const blend_mode mode = mode_of(options, remap_rounds);
    const bool remap_lookup = (options & SPRITE_REMAP) != 0;
    const vga_pixel *remap_row = ctx->remaps.tables[clamp(remap_offset, 0, VGA_REMAP_COUNT - 1)].data;
    const int transparent = src->transparent;
    const bool decimate = opacity < 255;
    const float decimate_limit = opacity / 255.0f;
    const bool flip_h = (flip_mode & FLIP_HORIZONTAL) != 0;
    const bool flip_v = (flip_mode & FLIP_VERTICAL) != 0;
    // no scaling, no mirror, no palette shift, no lookup: the source indexes as they are (all below the palette size)
    const bool plain = dst->w == tw && !flip_h && palette_offset == 0 && !remap_lookup;
    if(rg35xx_renderer_stats) {
        const long n = (long)(x1 - x0) * (y1 - y0);
        ctx->stats_sprites++;
        ctx->stats_pixels += n;
        // by path: the plain set, any other set, then the other modes; decimated ones apart
        ctx->stats_mode_pixels[decimate ? 5 : mode == MODE_SET ? (plain ? 0 : 1) : mode + 1] += n;
    }

    // the source column under each visible destination column's centre, once (the A9 has no divide instruction)
    int xmap[NATIVE_W];
    for(int x = x0; x < x1; x++) {
        const int i = x - dst->x;
        int sx = dst->w == tw ? i : (int)(((long)(2 * i + 1) * tw) / (2 * dst->w));
        xmap[x] = flip_h ? tw - 1 - sx : sx;
    }

    for(int y = y0; y < y1; y++) {
        const int j = y - dst->y;
        int sy = dst->h == th ? j : (int)(((long)(2 * j + 1) * th) / (2 * dst->h));
        if(flip_v) {
            sy = th - 1 - sy;
        }
        const vga_pixel *row = src->data + sy * tw;
        uint16_t *index_row = ctx->index + y * NATIVE_W;
        uint32_t *fx_row = ctx->fx + y * NATIVE_W;

        // the most common case: an unscaled, unflipped sprite's own indexes, set
        if(mode == MODE_SET && !decimate && plain) {
            const vga_pixel *in = row + (x0 - dst->x);
            for(int x = x0; x < x1; x++) {
                const int index = *in++;
                if(index != transparent) {
                    index_row[x] = (uint16_t)index;
                }
            }
            continue;
        }
        // the common case: an opaque sprite's pixels set, nothing else
        if(mode == MODE_SET && !decimate) {
            for(int x = x0; x < x1; x++) {
                int index = row[xmap[x]];
                if(index == transparent) {
                    continue;
                }
                if(index <= palette_limit) {
                    index = clamp(index + palette_offset, 0, palette_limit);
                }
                if(remap_lookup) {
                    index = remap_row[index & (VGA_PALETTE_SIZE - 1)];
                }
                index_row[x] = (uint16_t)min2(index, VGA_PALETTE_SIZE - 1);
            }
            continue;
        }

        for(int x = x0; x < x1; x++) {
            if(decimate) {
                float n = ctx->row_noise[y] + ctx->column_noise[x];
                if(n >= 1.0f) {
                    n -= 1.0f;
                }
                if(n > decimate_limit) {
                    continue;
                }
            }
            const int sx = xmap[x];

            int index;
            if(mode == MODE_SPRITE_SHADOW) {
                // four samples down the source, one row above to two below: the coverage, 0 to 4
                int coverage = 0;
                for(int k = -1; k <= 2; k++) {
                    const int r = sy + k;
                    coverage += (r >= 0 && r < th && src->data[r * tw + sx] != transparent);
                }
                if(coverage == 0) {
                    continue;
                }
                index = coverage;
            } else {
                index = row[sx];
                if(index == transparent) {
                    continue;
                }
                // palette offset and limit (the second player's colours, fonts)
                if(index <= palette_limit) {
                    index = clamp(index + palette_offset, 0, palette_limit);
                }
                if(remap_lookup) {
                    index = remap_row[index & (VGA_PALETTE_SIZE - 1)];
                }
            }

            const uint16_t old = index_row[x];
            const uint32_t fx = (old & FX_FLAG) ? fx_row[x] : 0;
            uint32_t new_fx = fx;
            switch(mode) {
                case MODE_SET:
                    index_row[x] = (uint16_t)min2(index, VGA_PALETTE_SIZE - 1);
                    continue;
                case MODE_REMAP:
                    new_fx = (fx & ~0xFFu) | FX_G(min2(remap_offset + remap_rounds * 19 + index, 255));
                    break;
                case MODE_SPRITE_SHADOW:
                    // only the remap channel is written, blended by maximum
                    if(remap_rounds > 0) {
                        const int g = min2(remap_offset + remap_rounds * 19 + index, 255);
                        if(g > FX_GET_G(fx)) {
                            new_fx = (fx & ~0xFFu) | FX_G(g);
                        }
                    }
                    break;
                case MODE_ADD:
                    new_fx = (fx & ~FX_A(0x3FF)) | FX_A(min2(index * 60, VGA_PALETTE_SIZE - 1));
                    break;
                case MODE_TRANSLUCENT:
                    // in arenas only indexes below 0x80 are brightness ramps (16 colours each); G and A cleared
                    if(index < 0x80) {
                        index = (index & 0xF0) + ((index & 0x0F) >> 1);
                    }
                    new_fx = FX_B(min2(index + 1, VGA_PALETTE_SIZE - 1));
                    break;
            }
            fx_row[x] = new_fx;
            index_row[x] = new_fx ? (uint16_t)(old | FX_FLAG) : (uint16_t)(old & INDEX_MASK);
        }
    }
}

static void move_target(void *userdata, int x, int y) {
    rg35xx_context *ctx = userdata;
    ctx->move_x = x;
    ctx->move_y = y;
}

static void render_prepare(void *userdata, unsigned framebuffer_options) {
    rg35xx_context *ctx = userdata;
    ctx->framebuffer_options = framebuffer_options;
}

static void flush_palette_and_remaps(rg35xx_context *ctx) {
    vga_index first, last;
    vga_palette *pal;
    if(vga_state_is_palette_dirty(&pal, &first, &last)) {
        for(vga_index i = max2(first, 0); i <= last && i < VGA_PALETTE_SIZE; i++) {
            const vga_color c = pal->colors[i];
            ctx->rgb565[i] = (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
            ctx->brightness[i] = (uint8_t)((c.r + c.g + c.b) / 3);
        }
        vga_state_mark_palette_flushed();
    }
    vga_remap_tables *tables;
    if(vga_state_is_remap_dirty(&tables)) {
        memcpy(&ctx->remaps, tables, sizeof(vga_remap_tables));
        vga_state_mark_remaps_flushed();
    }
}

// rgba.frag: the channels to the final palette index, then to RGB565
static void resolve(rg35xx_context *ctx) {
    const bool credits = (ctx->framebuffer_options & FBUFOPT_CREDITS) != 0;
    for(int p = 0; p < PIXELS; p++) {
        const unsigned v = ctx->index[p];
        if(!(v & FX_FLAG)) {
            ctx->picture[p] = ctx->rgb565[v];
            continue;
        }
        int idx = (int)(v & INDEX_MASK);
        const uint32_t fx = ctx->fx[p];
        const int enc = FX_GET_G(fx), tr = FX_GET_B(fx), idx_add = FX_GET_A(fx);
        idx = min2(idx + idx_add, VGA_PALETTE_SIZE - 1);
        if(credits) {
            ctx->picture[p] = ctx->rgb565[idx];
            continue;
        }
        if(tr > 0) {
            const int t = tr - 1;
            idx = t < 0x80 ? min2(t + (ctx->brightness[idx] >> 5), VGA_PALETTE_SIZE - 1) : t;
        }
        const int rounds = enc / 19;
        const vga_pixel *table = ctx->remaps.tables[enc % 19].data;
        for(int r = 0; r < rounds; r++) {
            idx = table[idx] & (VGA_PALETTE_SIZE - 1);
        }
        ctx->picture[p] = ctx->rgb565[idx];
    }
}

static void deliver_screenshot(rg35xx_context *ctx) {
    irect r = irect_create(0, 0, NATIVE_W, NATIVE_H);
    unsigned char *rgb = omf_malloc(PIXELS * 3);
    for(int p = 0; p < PIXELS; p++) {
        const uint16_t c = ctx->picture[p];
        rgb[p * 3 + 0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
        rgb[p * 3 + 1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
        rgb[p * 3 + 2] = (unsigned char)((c & 31) * 255 / 31);
    }
    ctx->screenshot_cb(&r, rgb, false);
    omf_free(rgb);
    ctx->screenshot_cb = NULL;
}

static void stats_frame(rg35xx_context *ctx, double work, double compose, double swapped) {
    ctx->stats_frames++;
    ctx->work_sum += work;
    ctx->compose_sum += compose;
    if(work > ctx->work_max) {
        ctx->work_max = work;
    }
    if(swapped - ctx->last_swap_ms > 1.5 * 1000.0 / 60) {
        ctx->stats_late++;
    }
    if(ctx->stats_since == 0) {
        ctx->stats_since = swapped;
    }
    if(swapped - ctx->stats_since >= 10000) {
        log_info("stats: %.1f fps, %ld frames, %ld late, work mean %.2f max %.2f ms (compose + present %.2f ms), "
                 "%ld sprites %ld pixels a frame (plain %ld set %ld remap %ld add %ld translucent+shadow %ld decimated %ld)",
                 ctx->stats_frames * 1000.0 / (swapped - ctx->stats_since), ctx->stats_frames, ctx->stats_late,
                 ctx->work_sum / ctx->stats_frames, ctx->work_max, ctx->compose_sum / ctx->stats_frames,
                 ctx->stats_sprites / ctx->stats_frames, ctx->stats_pixels / ctx->stats_frames,
                 ctx->stats_mode_pixels[0] / ctx->stats_frames, ctx->stats_mode_pixels[1] / ctx->stats_frames,
                 ctx->stats_mode_pixels[2] / ctx->stats_frames, ctx->stats_mode_pixels[3] / ctx->stats_frames,
                 ctx->stats_mode_pixels[4] / ctx->stats_frames, ctx->stats_mode_pixels[5] / ctx->stats_frames);
        memset(ctx->stats_mode_pixels, 0, sizeof ctx->stats_mode_pixels);
        ctx->stats_frames = ctx->stats_late = ctx->stats_sprites = ctx->stats_pixels = 0;
        ctx->work_sum = ctx->work_max = ctx->compose_sum = 0;
        ctx->stats_since = swapped;
    }
}

static void render_finish(void *userdata) {
    rg35xx_context *ctx = userdata;
    const double start = now_ms();

    flush_palette_and_remaps(ctx);
    resolve(ctx);
    if(ctx->screenshot_cb) {
        deliver_screenshot(ctx);
    }

    glBindTexture(GL_TEXTURE_2D, ctx->textures[ctx->next_texture]);
    ctx->next_texture = (ctx->next_texture + 1) % 3;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NATIVE_W, NATIVE_H, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, ctx->picture);

    // 2x in the middle of the panel; a screen shake moves it (as the GL3 viewport moves: up for a positive y)
    const int x = (SCREEN_W - 2 * NATIVE_W) / 2 + 2 * ctx->move_x;
    const int y = (SCREEN_H - 2 * NATIVE_H) / 2 - 2 * ctx->move_y;
    const GLfloat l = 2.0f * x / SCREEN_W - 1, r = 2.0f * (x + 2 * NATIVE_W) / SCREEN_W - 1;
    const GLfloat t = 1 - 2.0f * y / SCREEN_H, b = 1 - 2.0f * (y + 2 * NATIVE_H) / SCREEN_H;
    const GLfloat quad[] = {l, t, 0, 0, r, t, 1, 0, l, b, 0, 1, r, b, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, quad + 2);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    const double compose = now_ms() - start;
    SDL_GL_SwapWindow(ctx->window); // returns at the vblank that shows it
    const double swapped = now_ms();
    if(rg35xx_renderer_stats) {
        stats_frame(ctx, start - ctx->last_swap_ms + compose, compose, swapped);
    }
    ctx->last_swap_ms = swapped;
}

static void render_area_prepare(void *userdata, const irect *area) {
    rg35xx_context *ctx = userdata;
    ctx->area = *area;
}

// the area's palette indexes as a surface (the GL3 renderer reads back its R channel the same way)
static void render_area_finish(void *userdata, surface *dst) {
    rg35xx_context *ctx = userdata;
    flush_palette_and_remaps(ctx);
    const int x0 = clamp(ctx->area.x, 0, NATIVE_W), x1 = clamp(ctx->area.x + ctx->area.w, 0, NATIVE_W);
    const int y0 = clamp(ctx->area.y, 0, NATIVE_H), y1 = clamp(ctx->area.y + ctx->area.h, 0, NATIVE_H);
    const int w = x1 - x0, h = y1 - y0;
    surface_create(dst, w, h);
    for(int y = 0; y < h; y++) {
        for(int x = 0; x < w; x++) {
            dst->data[y * w + x] = (vga_pixel)(ctx->index[(y0 + y) * NATIVE_W + x0 + x] & INDEX_MASK);
        }
    }
    dst->transparent = -1;
}

static void capture_screen(void *userdata, video_screenshot_signal screenshot_cb) {
    rg35xx_context *ctx = userdata;
    ctx->screenshot_cb = screenshot_cb;
}

static void signal_scene_change(void *userdata) {
}

static void signal_draw_atlas(void *userdata, bool toggle) {
}

static void renderer_create(renderer *r) {
    r->ctx = omf_calloc(1, sizeof(rg35xx_context));
}

static void renderer_destroy(renderer *r) {
    omf_free(r->ctx);
}

void rg35xx_renderer_set_callbacks(renderer *r) {
    r->is_available = is_available;
    r->get_description = get_description;
    r->get_name = get_name;

    r->create = renderer_create;
    r->destroy = renderer_destroy;

    r->setup_context = setup_context;
    r->get_context_state = get_context_state;
    r->reset_context_with = reset_context_with;
    r->reset_context = reset_context;
    r->close_context = close_context;

    r->draw_surface = draw_surface;
    r->move_target = move_target;
    r->render_prepare = render_prepare;
    r->render_finish = render_finish;
    r->render_area_prepare = render_area_prepare;
    r->render_area_finish = render_area_finish;

    r->capture_screen = capture_screen;
    r->signal_scene_change = signal_scene_change;
    r->signal_draw_atlas = signal_draw_atlas;
}
