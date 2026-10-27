/*
 * rnd_gl.c redundant-state caching, through the public rnd entry points
 * and a recording GLES state machine. Draw snapshots are taken from the
 * simulated GL state, not from the production caches.
 *
 * Build with TEST_EXPECT_STATE_CACHE=1 for IGN_GLES_STATE_CACHE=1, or 0
 * for the feature-off regression control.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "GLES3/gl3.h"
#include "rnd.h"

#ifndef TEST_EXPECT_STATE_CACHE
#error "define TEST_EXPECT_STATE_CACHE=0 or 1"
#endif

#if defined(IGN_GLES_FRAMEBUFFER_FETCH) && IGN_GLES_FRAMEBUFFER_FETCH == 1
#define TEST_HAVE_FETCH_BUILD 1
#else
#define TEST_HAVE_FETCH_BUILD 0
#endif

enum {
    MAX_PROGRAMS = 128,
    MAX_SHADERS = 128,
    MAX_UNITS = 64,
    MAX_VAOS = 64,
    MAX_BUFFERS = 64,
    MAX_TEXTURES = 128,
    MAX_FRAMEBUFFERS = 16,
    MAX_RENDERBUFFERS = 16,
    MAX_ATTRS = 4,
    MAX_BUFFER_BYTES = 512,
    MAX_TEXTURE_BYTES = 1024,
    MAX_SHADER_SOURCE = 4096,
    MAX_EXTENSIONS = 4,
    UNIFORM_COUNT = 7,
    UNIFORM_FLOATS = 16,
    SAMPLER_LOCATION = 100
};

struct uniform_value {
    int valid;
    int kind;
    GLint i[UNIFORM_FLOATS];
    GLfloat f[UNIFORM_FLOATS];
};

struct draw_attrib {
    int enabled;
    GLint size;
    GLsizei stride;
    GLintptr offset;
    GLuint buffer;
    size_t bytes;
    unsigned char data[MAX_BUFFER_BYTES];
};

struct draw_snapshot {
    GLuint program;
    GLuint vao;
    GLuint array_buffer;
    int depth_enabled;
    GLenum depth_func;
    GLboolean depth_mask;
    int cull_enabled;
    GLenum cull_face;
    GLenum front_face;
    int poly_enabled;
    GLfloat poly_factor;
    GLfloat poly_units;
    GLenum active_texture;
    GLuint tex2d[MAX_UNITS];
    GLuint tex_array[MAX_UNITS];
    struct uniform_value uniforms[UNIFORM_COUNT];
    GLenum mode;
    GLint first;
    GLsizei count;
    struct draw_attrib attrs[MAX_ATTRS];
};

struct attrib_record {
    int enabled;
    GLint size;
    GLenum type;
    GLsizei stride;
    GLintptr offset;
    GLuint buffer;
};

struct vao_record {
    int valid;
    struct attrib_record attrib[MAX_ATTRS];
};

struct buffer_record {
    int valid;
    size_t size;
    int truncated;
    unsigned char data[MAX_BUFFER_BYTES];
};

struct texture_record {
    int valid;
    GLenum target;
    GLenum format;
    GLsizei w, h, depth;
    size_t bytes;
    int truncated;
    unsigned char data[MAX_TEXTURE_BYTES];
};

struct framebuffer_record {
    int valid;
    GLuint color, depth;
};

struct renderbuffer_record {
    int valid;
    GLenum format;
    GLsizei w, h;
};

struct copy_record {
    int valid, unit;
    GLuint read_fbo, texture;
    GLenum target;
    GLint xoffset, yoffset, x, y;
    GLsizei w, h;
};

struct blit_record {
    int valid;
    GLuint read_fbo, draw_fbo;
    GLint sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1;
    GLbitfield mask;
    GLenum filter;
};

static struct {
    int use_program, bind_vao, active_texture;
    int bind_tex2d, bind_tex_array;
    int enable_depth, disable_depth, depth_func, depth_mask;
    int enable_cull, disable_cull, cull_face, front_face;
    int enable_poly, disable_poly, polygon_offset;
    int uniform1i, uniform2f, uniform4f, uniform_matrix;
    int draws;
} s_calls;

static struct {
    GLuint current_program, current_vao;
    int depth_enabled;
    GLenum depth_func;
    GLboolean depth_mask;
    int cull_enabled;
    GLenum cull_face;
    GLenum front_face;
    int poly_enabled;
    GLfloat poly_factor, poly_units;
    GLenum active_texture;
    GLuint tex2d[MAX_UNITS], tex_array[MAX_UNITS];
    struct uniform_value uniforms[MAX_PROGRAMS][UNIFORM_COUNT];
    struct draw_snapshot last_draw;
} s_gl;

static GLuint s_array_buffer, s_bound_renderbuffer;
static GLuint s_read_fbo, s_draw_fbo;
static GLint s_viewport[4];
static GLint s_unpack_alignment, s_unpack_row_length;
static struct buffer_record s_buffers[MAX_BUFFERS];
static struct vao_record s_vaos[MAX_VAOS];
static struct texture_record s_textures[MAX_TEXTURES];
static struct framebuffer_record s_framebuffers[MAX_FRAMEBUFFERS];
static struct renderbuffer_record s_renderbuffers[MAX_RENDERBUFFERS];
static struct copy_record s_last_copy;
static struct blit_record s_last_blit;

static GLuint s_next_shader = 1000, s_next_program = 5000;
static GLuint s_next_vao = 100, s_next_buffer = 200, s_next_texture = 600;
static GLuint s_next_framebuffer = 300, s_next_renderbuffer = 400;
static GLenum s_shader_types[MAX_SHADERS];
static int s_fail_shader, s_fail_fragment, s_fail_link;
static int s_fail_dyn_fragment, s_fail_fetch_shader;
static struct {
    int dyn_fragment;
    int fetch_variant;
} s_shader_info[MAX_SHADERS];
static const char *s_extensions[MAX_EXTENSIONS];
static int s_extension_count;
static int s_failures;

enum { MAX_SOURCE_SPANS = 32 };

struct source_span {
    const unsigned char *pixels;
    size_t bytes;
};

static struct source_span s_sources[MAX_SOURCE_SPANS];
static int s_source_count;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++; \
        } \
    } while (0)

/* Declare the storage behind an upload source so the recorder can refuse
 * to read past it instead of overreading a test stack buffer. */
static void source_expect(const void *pixels, size_t bytes)
{
    if (pixels == NULL) {
        return;
    }
    if (s_source_count >= MAX_SOURCE_SPANS) {
        fprintf(stderr, "test_perf_state_cache: too many upload sources\n");
        s_failures++;
        return;
    }
    s_sources[s_source_count].pixels = (const unsigned char *)pixels;
    s_sources[s_source_count].bytes = bytes;
    s_source_count++;
}

static int source_read_ok(const unsigned char *pixels, size_t bytes)
{
    uintptr_t p = (uintptr_t)pixels;
    int i;

    for (i = 0; i < s_source_count; i++) {
        uintptr_t base = (uintptr_t)s_sources[i].pixels;
        size_t offset;

        if (p < base) {
            continue;
        }
        offset = (size_t)(p - base);
        if (offset <= s_sources[i].bytes && bytes <= s_sources[i].bytes - offset) {
            return 1;
        }
    }
    fprintf(stderr,
            "FAIL %s:%d: recorder read of %zu bytes at %p is outside every registered "
            "source\n",
            __FILE__, __LINE__, bytes, (const void *)pixels);
    s_failures++;
    return 0;
}

#if TEST_EXPECT_STATE_CACHE
#define REPEATED_ONCE 1
#define REPEATED_ZERO 0
#else
#define REPEATED_ONCE 2
#define REPEATED_ZERO 0
#endif

static void calls_clear(void)
{
    memset(&s_calls, 0, sizeof(s_calls));
}

static void gl_defaults(void)
{
    memset(&s_gl, 0, sizeof(s_gl));
    s_gl.depth_mask = GL_TRUE;
    s_gl.depth_func = GL_LESS;
    s_gl.cull_face = GL_BACK;
    s_gl.front_face = GL_CCW;
    s_gl.active_texture = GL_TEXTURE0;
}

static void fake_reset(void)
{
    gl_defaults();
    calls_clear();
    s_next_shader = 1000;
    s_next_program = 5000;
    s_next_vao = 100;
    s_next_buffer = 200;
    s_next_texture = 600;
    s_next_framebuffer = 300;
    s_next_renderbuffer = 400;
    memset(s_shader_types, 0, sizeof(s_shader_types));
    memset(s_shader_info, 0, sizeof(s_shader_info));
    memset(s_extensions, 0, sizeof(s_extensions));
    s_extension_count = 0;
    s_array_buffer = 0;
    s_bound_renderbuffer = 0;
    s_read_fbo = 0;
    s_draw_fbo = 0;
    memset(s_viewport, 0, sizeof(s_viewport));
    s_unpack_alignment = 4;
    s_unpack_row_length = 0;
    memset(s_buffers, 0, sizeof(s_buffers));
    memset(s_vaos, 0, sizeof(s_vaos));
    memset(s_textures, 0, sizeof(s_textures));
    memset(s_framebuffers, 0, sizeof(s_framebuffers));
    memset(s_renderbuffers, 0, sizeof(s_renderbuffers));
    memset(&s_last_copy, 0, sizeof(s_last_copy));
    memset(&s_last_blit, 0, sizeof(s_last_blit));
    s_source_count = 0;
    s_fail_shader = 0;
    s_fail_fragment = 0;
    s_fail_link = 0;
    s_fail_dyn_fragment = 0;
    s_fail_fetch_shader = 0;
}

#if TEST_HAVE_FETCH_BUILD
static void fake_extensions(const char *const *names, int count)
{
    int i;

    s_extension_count = count < MAX_EXTENSIONS ? count : MAX_EXTENSIONS;
    for (i = 0; i < s_extension_count; i++) {
        s_extensions[i] = names[i];
    }
}
#endif

static int program_index(GLuint program)
{
    return program >= 5000 && program < 5000 + MAX_PROGRAMS ? (int)(program - 5000) : -1;
}

static int unit_index(GLenum unit)
{
    return unit >= GL_TEXTURE0 && unit < GL_TEXTURE0 + MAX_UNITS ? (int)(unit - GL_TEXTURE0) : -1;
}

static int vao_index(GLuint vao)
{
    return vao >= 100 && vao < 100 + MAX_VAOS ? (int)(vao - 100) : -1;
}

static int buffer_index(GLuint buffer)
{
    return buffer >= 200 && buffer < 200 + MAX_BUFFERS ? (int)(buffer - 200) : -1;
}

static int texture_index(GLuint texture)
{
    return texture >= 600 && texture < 600 + MAX_TEXTURES ? (int)(texture - 600) : -1;
}

static int framebuffer_index(GLuint framebuffer)
{
    return framebuffer >= 300 && framebuffer < 300 + MAX_FRAMEBUFFERS
               ? (int)(framebuffer - 300) : -1;
}

static int renderbuffer_index(GLuint renderbuffer)
{
    return renderbuffer >= 400 && renderbuffer < 400 + MAX_RENDERBUFFERS
               ? (int)(renderbuffer - 400) : -1;
}

static struct vao_record *vao_record(GLuint vao)
{
    int i = vao_index(vao);

    return i < 0 ? NULL : &s_vaos[i];
}

static struct buffer_record *buffer_record(GLuint buffer)
{
    int i = buffer_index(buffer);

    return i < 0 ? NULL : &s_buffers[i];
}

static struct texture_record *texture_record(GLuint texture)
{
    int i = texture_index(texture);

    return i < 0 ? NULL : &s_textures[i];
}

static struct framebuffer_record *framebuffer_record(GLuint framebuffer)
{
    int i = framebuffer_index(framebuffer);

    return i < 0 ? NULL : &s_framebuffers[i];
}

static struct renderbuffer_record *renderbuffer_record(GLuint renderbuffer)
{
    int i = renderbuffer_index(renderbuffer);

    return i < 0 ? NULL : &s_renderbuffers[i];
}

static GLuint bound_texture(GLenum target)
{
    int u = unit_index(s_gl.active_texture);

    if (u < 0) {
        return 0;
    }
    if (target == GL_TEXTURE_2D) {
        return s_gl.tex2d[u];
    }
    return target == GL_TEXTURE_2D_ARRAY ? s_gl.tex_array[u] : 0;
}

static size_t format_bytes(GLenum format)
{
    return format == GL_RG ? 2 : format == GL_RGB ? 3 : format == GL_RGBA ? 4 : 1;
}

static size_t unpack_row_bytes(int width, size_t bpp)
{
    size_t row;

    if (s_unpack_row_length > 0) {
        return (size_t)s_unpack_row_length * bpp;
    }
    row = (size_t)width * bpp;
    if (s_unpack_alignment > 1) {
        row = (row + (size_t)s_unpack_alignment - 1) & ~((size_t)s_unpack_alignment - 1);
    }
    return row;
}

static void texture_image(GLuint texture, GLenum target, GLenum format, GLsizei w, GLsizei h,
                          GLsizei depth, const void *pixels)
{
    struct texture_record *r = texture_record(texture);
    size_t bpp, bytes;
    int z, y;

    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->valid = 1;
    r->target = target;
    r->format = format;
    r->w = w;
    r->h = h;
    r->depth = depth;
    if (w <= 0 || h <= 0 || depth <= 0) {
        return;
    }
    bpp = format_bytes(format);
    bytes = (size_t)w * (size_t)h * (size_t)depth * bpp;
    r->bytes = bytes;
    if (pixels == NULL) {
        return;
    }
    if (bytes > MAX_TEXTURE_BYTES) {
        r->truncated = 1;
        return;
    }
    for (z = 0; z < depth; z++) {
        for (y = 0; y < h; y++) {
            const unsigned char *src = (const unsigned char *)pixels +
                                       ((size_t)z * (size_t)h + (size_t)y) *
                                           unpack_row_bytes(w, bpp);
            unsigned char *dst = r->data + (((size_t)z * (size_t)h + (size_t)y) *
                                            (size_t)w) * bpp;

            if (!source_read_ok(src, (size_t)w * bpp)) {
                return;
            }
            memcpy(dst, src, (size_t)w * bpp);
        }
    }
}

static void texture_subimage(GLuint texture, GLenum format, GLsizei xoffset, GLsizei yoffset,
                             GLsizei w, GLsizei h, const void *pixels)
{
    struct texture_record *r = texture_record(texture);
    size_t bpp;
    int y;

    if (r == NULL || !r->valid || pixels == NULL) {
        return;
    }
    if (xoffset < 0 || yoffset < 0 || w <= 0 || h <= 0 || xoffset + w > r->w ||
        yoffset + h > r->h) {
        return;
    }
    bpp = format_bytes(format);
    for (y = 0; y < h; y++) {
        const unsigned char *src = (const unsigned char *)pixels +
                                   (size_t)y * unpack_row_bytes(w, bpp);
        unsigned char *dst = r->data +
                             (((size_t)(yoffset + y) * (size_t)r->w + (size_t)xoffset) * bpp);

        if (!source_read_ok(src, (size_t)w * bpp)) {
            return;
        }
        memcpy(dst, src, (size_t)w * bpp);
    }
}

static int uniform_index(const char *name)
{
    static const char *const names[UNIFORM_COUNT] = {
        "u_lsize", "u_off", "u_mode", "u_rect", "u_matrix", "u_use_table", "u_size"
    };
    int i;

    for (i = 0; i < UNIFORM_COUNT; i++) {
        if (strcmp(name, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static struct uniform_value *current_uniform(GLint location)
{
    int p = program_index(s_gl.current_program);
    int u = (int)location - 1;

    if (p < 0 || u < 0 || u >= UNIFORM_COUNT) {
        return NULL;
    }
    return &s_gl.uniforms[p][u];
}

static void snapshot_draw(void)
{
    int p = program_index(s_gl.current_program);
    struct vao_record *vao = vao_record(s_gl.current_vao);
    int i;

    memset(&s_gl.last_draw, 0, sizeof(s_gl.last_draw));
    s_gl.last_draw.program = s_gl.current_program;
    s_gl.last_draw.vao = s_gl.current_vao;
    s_gl.last_draw.array_buffer = s_array_buffer;
    s_gl.last_draw.depth_enabled = s_gl.depth_enabled;
    s_gl.last_draw.depth_func = s_gl.depth_func;
    s_gl.last_draw.depth_mask = s_gl.depth_mask;
    s_gl.last_draw.cull_enabled = s_gl.cull_enabled;
    s_gl.last_draw.cull_face = s_gl.cull_face;
    s_gl.last_draw.front_face = s_gl.front_face;
    s_gl.last_draw.poly_enabled = s_gl.poly_enabled;
    s_gl.last_draw.poly_factor = s_gl.poly_factor;
    s_gl.last_draw.poly_units = s_gl.poly_units;
    s_gl.last_draw.active_texture = s_gl.active_texture;
    memcpy(s_gl.last_draw.tex2d, s_gl.tex2d, sizeof(s_gl.tex2d));
    memcpy(s_gl.last_draw.tex_array, s_gl.tex_array, sizeof(s_gl.tex_array));
    if (p >= 0) {
        memcpy(s_gl.last_draw.uniforms, s_gl.uniforms[p], sizeof(s_gl.last_draw.uniforms));
    }
    if (vao != NULL) {
        for (i = 0; i < MAX_ATTRS; i++) {
            struct buffer_record *b;
            size_t bytes;

            s_gl.last_draw.attrs[i].enabled = vao->attrib[i].enabled;
            s_gl.last_draw.attrs[i].size = vao->attrib[i].size;
            s_gl.last_draw.attrs[i].stride = vao->attrib[i].stride;
            s_gl.last_draw.attrs[i].offset = vao->attrib[i].offset;
            s_gl.last_draw.attrs[i].buffer = vao->attrib[i].buffer;
            b = buffer_record(vao->attrib[i].buffer);
            if (b == NULL || !b->valid) {
                continue;
            }
            bytes = b->size < MAX_BUFFER_BYTES ? b->size : MAX_BUFFER_BYTES;
            s_gl.last_draw.attrs[i].bytes = bytes;
            memcpy(s_gl.last_draw.attrs[i].data, b->data, bytes);
        }
    }
}

/* ------------------------------------------------------------ GL fakes */

GLuint glCreateShader(GLenum type)
{
    GLuint shader = s_next_shader++;

    if (shader >= 1000 && shader < 1000 + MAX_SHADERS) {
        s_shader_types[shader - 1000] = type;
    }
    return shader;
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                    const GLint *length)
{
    char source[MAX_SHADER_SOURCE];
    size_t used = 0;
    GLsizei i;

    if (shader < 1000 || shader >= 1000 + MAX_SHADERS) {
        return;
    }
    memset(source, 0, sizeof(source));
    for (i = 0; i < count && used < sizeof(source) - 1; i++) {
        size_t n = length != NULL && length[i] >= 0 ? (size_t)length[i]
                                                    : strlen(string[i]);

        if (n > sizeof(source) - 1 - used) {
            n = sizeof(source) - 1 - used;
        }
        memcpy(source + used, string[i], n);
        used += n;
    }
    s_shader_info[shader - 1000].fetch_variant =
        strstr(source, "GL_EXT_shader_framebuffer_fetch") != NULL;
    s_shader_info[shader - 1000].dyn_fragment = strstr(source, "u_use_table") != NULL;
}

void glCompileShader(GLuint shader)
{
    (void)shader;
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
    int fragment = shader >= 1000 && shader < 1000 + MAX_SHADERS &&
                   s_shader_types[shader - 1000] == GL_FRAGMENT_SHADER;
    int shader_fails =
        shader >= 1000 && shader < 1000 + MAX_SHADERS &&
        ((s_fail_dyn_fragment && s_shader_info[shader - 1000].dyn_fragment) ||
         (s_fail_fetch_shader && s_shader_info[shader - 1000].fetch_variant));

    *params = pname == GL_COMPILE_STATUS && !s_fail_shader &&
              !(s_fail_fragment && fragment) && !shader_fails ? GL_TRUE : GL_FALSE;
}

void glGetShaderInfoLog(GLuint shader, GLsizei buf_size, GLsizei *length, GLchar *info_log)
{
    static const char log[] = "fake compile failure";
    size_t n;

    (void)shader;
    n = buf_size > (GLsizei)sizeof(log) ? sizeof(log) - 1 : (size_t)buf_size - 1;
    if (buf_size <= 0) {
        if (length != NULL) {
            *length = 0;
        }
        return;
    }
    memcpy(info_log, log, n);
    info_log[n] = '\0';
    if (length != NULL) {
        *length = (GLsizei)n;
    }
}

void glDeleteShader(GLuint shader)
{
    (void)shader;
}

GLuint glCreateProgram(void)
{
    GLuint program = s_next_program++;

    if (program_index(program) >= 0) {
        memset(s_gl.uniforms[program_index(program)], 0,
               sizeof(s_gl.uniforms[program_index(program)]));
    }
    return program;
}

void glAttachShader(GLuint program, GLuint shader)
{
    (void)program;
    (void)shader;
}

void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name)
{
    (void)program;
    (void)index;
    (void)name;
}

void glBindFragDataLocation(GLuint program, GLuint index, const GLchar *name)
{
    (void)program;
    (void)index;
    (void)name;
}

void glLinkProgram(GLuint program)
{
    (void)program;
}

void glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
    (void)program;
    *params = pname == GL_LINK_STATUS && !s_fail_link ? GL_TRUE : GL_FALSE;
}

void glDeleteProgram(GLuint program)
{
    if (s_gl.current_program == program) {
        s_gl.current_program = 0;
    }
}

GLint glGetUniformLocation(GLuint program, const GLchar *name)
{
    int u = uniform_index(name);

    (void)program;
    if (u >= 0) {
        return u + 1;
    }
    if (strcmp(name, "u_src") == 0 || strcmp(name, "u_index") == 0 ||
        strcmp(name, "u_pages") == 0) {
        return SAMPLER_LOCATION;
    }
    if (strcmp(name, "u_table") == 0 || strcmp(name, "u_palette") == 0) {
        return SAMPLER_LOCATION + 1;
    }
    if (strcmp(name, "u_rows") == 0 || strcmp(name, "u_snap") == 0) {
        return SAMPLER_LOCATION + 2;
    }
    if (strcmp(name, "u_dst") == 0) {
        return SAMPLER_LOCATION + 3;
    }
    return -1;
}

void glUseProgram(GLuint program)
{
    s_calls.use_program++;
    s_gl.current_program = program;
}

void glGenVertexArrays(GLsizei n, GLuint *arrays)
{
    GLsizei i;

    for (i = 0; i < n; i++) {
        arrays[i] = s_next_vao++;
    }
}

void glBindVertexArray(GLuint array)
{
    struct vao_record *v = vao_record(array);

    s_calls.bind_vao++;
    s_gl.current_vao = array;
    if (v != NULL) {
        v->valid = 1;
    }
}

void glDeleteVertexArrays(GLsizei n, const GLuint *arrays)
{
    GLsizei i;

    for (i = 0; i < n; i++) {
        if (s_gl.current_vao == arrays[i]) {
            s_gl.current_vao = 0;
        }
        {
            struct vao_record *v = vao_record(arrays[i]);

            if (v != NULL) {
                memset(v, 0, sizeof(*v));
            }
        }
    }
}

void glGenBuffers(GLsizei n, GLuint *buffers)
{
    GLsizei i;

    for (i = 0; i < n; i++) {
        buffers[i] = s_next_buffer++;
    }
}

void glGenTextures(GLsizei n, GLuint *textures)
{
    GLsizei i;

    for (i = 0; i < n; i++) {
        textures[i] = s_next_texture++;
    }
}

void glActiveTexture(GLenum texture)
{
    s_calls.active_texture++;
    s_gl.active_texture = texture;
}

void glBindTexture(GLenum target, GLuint texture)
{
    int u = unit_index(s_gl.active_texture);

    if (u < 0) {
        return;
    }
    if (target == GL_TEXTURE_2D) {
        s_calls.bind_tex2d++;
        s_gl.tex2d[u] = texture;
    } else if (target == GL_TEXTURE_2D_ARRAY) {
        s_calls.bind_tex_array++;
        s_gl.tex_array[u] = texture;
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    GLsizei i;
    int u;

    for (i = 0; i < n; i++) {
        struct texture_record *r = texture_record(textures[i]);

        if (r != NULL) {
            memset(r, 0, sizeof(*r));
        }
        for (u = 0; u < MAX_UNITS; u++) {
            if (s_gl.tex2d[u] == textures[i]) {
                s_gl.tex2d[u] = 0;
            }
            if (s_gl.tex_array[u] == textures[i]) {
                s_gl.tex_array[u] = 0;
            }
        }
    }
}

void glEnable(GLenum cap)
{
    if (cap == GL_DEPTH_TEST) {
        s_calls.enable_depth++;
        s_gl.depth_enabled = 1;
    } else if (cap == GL_CULL_FACE) {
        s_calls.enable_cull++;
        s_gl.cull_enabled = 1;
    } else if (cap == GL_POLYGON_OFFSET_FILL) {
        s_calls.enable_poly++;
        s_gl.poly_enabled = 1;
    }
}

void glDisable(GLenum cap)
{
    if (cap == GL_DEPTH_TEST) {
        s_calls.disable_depth++;
        s_gl.depth_enabled = 0;
    } else if (cap == GL_CULL_FACE) {
        s_calls.disable_cull++;
        s_gl.cull_enabled = 0;
    } else if (cap == GL_POLYGON_OFFSET_FILL) {
        s_calls.disable_poly++;
        s_gl.poly_enabled = 0;
    }
}

void glDepthFunc(GLenum func)
{
    s_calls.depth_func++;
    s_gl.depth_func = func;
}

void glDepthMask(GLboolean flag)
{
    s_calls.depth_mask++;
    s_gl.depth_mask = flag;
}

void glCullFace(GLenum mode)
{
    s_calls.cull_face++;
    s_gl.cull_face = mode;
}

void glFrontFace(GLenum mode)
{
    s_calls.front_face++;
    s_gl.front_face = mode;
}

void glPolygonOffset(GLfloat factor, GLfloat units)
{
    s_calls.polygon_offset++;
    s_gl.poly_factor = factor;
    s_gl.poly_units = units;
}

void glUniform1i(GLint location, GLint v0)
{
    struct uniform_value *u = current_uniform(location);

    s_calls.uniform1i++;
    if (u != NULL) {
        u->valid = 1;
        u->kind = 1;
        u->i[0] = v0;
    }
}

void glUniform2f(GLint location, GLfloat v0, GLfloat v1)
{
    struct uniform_value *u = current_uniform(location);

    s_calls.uniform2f++;
    if (u != NULL) {
        u->valid = 1;
        u->kind = 2;
        u->f[0] = v0;
        u->f[1] = v1;
    }
}

void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)
{
    struct uniform_value *u = current_uniform(location);

    s_calls.uniform4f++;
    if (u != NULL) {
        u->valid = 1;
        u->kind = 4;
        u->f[0] = v0;
        u->f[1] = v1;
        u->f[2] = v2;
        u->f[3] = v3;
    }
}

void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *value)
{
    struct uniform_value *u = current_uniform(location);

    s_calls.uniform_matrix++;
    if (u != NULL) {
        u->valid = 1;
        u->kind = 16;
        u->i[0] = count;
        u->i[1] = transpose;
        memcpy(u->f, value, 16 * sizeof(GLfloat));
    }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    s_calls.draws++;
    snapshot_draw();
    s_gl.last_draw.mode = mode;
    s_gl.last_draw.first = first;
    s_gl.last_draw.count = count;
}

void glBindBuffer(GLenum target, GLuint buffer)
{
    if (target == GL_ARRAY_BUFFER) {
        s_array_buffer = buffer;
    }
}
void glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    struct framebuffer_record *f = framebuffer_record(framebuffer);

    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) {
        s_read_fbo = framebuffer;
    }
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
        s_draw_fbo = framebuffer;
    }
    if (f != NULL) {
        f->valid = 1;
    }
}
void glBindRenderbuffer(GLenum target, GLuint renderbuffer)
{
    (void)target;
    s_bound_renderbuffer = renderbuffer;
}
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    struct buffer_record *b = buffer_record(s_array_buffer);
    size_t bytes = size > 0 ? (size_t)size : 0;
    size_t stored = bytes < MAX_BUFFER_BYTES ? bytes : MAX_BUFFER_BYTES;

    (void)target;
    (void)usage;
    if (b == NULL) {
        return;
    }
    b->valid = 1;
    b->size = bytes;
    b->truncated = bytes > MAX_BUFFER_BYTES;
    memset(b->data, 0, sizeof(b->data));
    if (data != NULL) {
        memcpy(b->data, data, stored);
    }
}
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
    struct buffer_record *b = buffer_record(s_array_buffer);
    size_t start = offset > 0 ? (size_t)offset : 0;
    size_t bytes = size > 0 ? (size_t)size : 0;
    size_t stored;

    (void)target;
    if (b == NULL || !b->valid || data == NULL) {
        return;
    }
    if (start >= MAX_BUFFER_BYTES) {
        b->truncated = 1;
        return;
    }
    stored = bytes < MAX_BUFFER_BYTES - start ? bytes : MAX_BUFFER_BYTES - start;
    memcpy(b->data + start, data, stored);
    if (stored != bytes) {
        b->truncated = 1;
    }
}
void glClear(GLbitfield mask) { (void)mask; }
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{ (void)r; (void)g; (void)b; (void)a; }
void glClearDepthf(GLfloat depth) { (void)depth; }
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLint x, GLint y, GLsizei width, GLsizei height)
{
    (void)level;
    s_last_copy.valid = 1;
    s_last_copy.unit = unit_index(s_gl.active_texture);
    s_last_copy.read_fbo = s_read_fbo;
    s_last_copy.texture = bound_texture(target);
    s_last_copy.target = target;
    s_last_copy.xoffset = xoffset;
    s_last_copy.yoffset = yoffset;
    s_last_copy.x = x;
    s_last_copy.y = y;
    s_last_copy.w = width;
    s_last_copy.h = height;
}
void glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
    GLsizei i;

    for (i = 0; i < n; i++) {
        struct buffer_record *b = buffer_record(buffers[i]);

        if (b != NULL) {
            memset(b, 0, sizeof(*b));
        }
        if (s_array_buffer == buffers[i]) {
            s_array_buffer = 0;
        }
    }
}
void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers)
{
    GLsizei i;

    for (i = 0; i < n; i++) {
        struct framebuffer_record *f = framebuffer_record(framebuffers[i]);

        if (f != NULL) {
            memset(f, 0, sizeof(*f));
        }
        if (s_read_fbo == framebuffers[i]) {
            s_read_fbo = 0;
        }
        if (s_draw_fbo == framebuffers[i]) {
            s_draw_fbo = 0;
        }
    }
}
void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers)
{
    GLsizei i;

    for (i = 0; i < n; i++) {
        struct renderbuffer_record *r = renderbuffer_record(renderbuffers[i]);

        if (r != NULL) {
            memset(r, 0, sizeof(*r));
        }
        if (s_bound_renderbuffer == renderbuffers[i]) {
            s_bound_renderbuffer = 0;
        }
    }
}
void glEnableVertexAttribArray(GLuint index)
{
    struct vao_record *v = vao_record(s_gl.current_vao);

    if (v != NULL && index < MAX_ATTRS) {
        v->attrib[index].enabled = 1;
    }
}
void glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                               GLenum renderbuffertarget, GLuint renderbuffer)
{
    struct framebuffer_record *f;

    (void)target;
    (void)renderbuffertarget;
    f = framebuffer_record(s_draw_fbo);
    if (f != NULL && attachment == GL_DEPTH_ATTACHMENT) {
        f->depth = renderbuffer;
    }
}
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                            GLuint texture, GLint level)
{
    struct framebuffer_record *f;

    (void)target;
    (void)textarget;
    (void)level;
    f = framebuffer_record(s_draw_fbo);
    if (f != NULL && attachment == GL_COLOR_ATTACHMENT0) {
        f->color = texture;
    }
}
void glGenFramebuffers(GLsizei n, GLuint *framebuffers)
{
    GLsizei i;

    for (i = 0; i < n; i++) {
        framebuffers[i] = s_next_framebuffer++;
    }
}
void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers)
{
    GLsizei i;

    for (i = 0; i < n; i++) {
        renderbuffers[i] = s_next_renderbuffer++;
    }
}
void glPixelStorei(GLenum pname, GLint param)
{
    if (pname == GL_UNPACK_ALIGNMENT) {
        s_unpack_alignment = param;
    } else if (pname == GL_UNPACK_ROW_LENGTH) {
        s_unpack_row_length = param;
    }
}
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                  GLenum type, void *pixels)
{ (void)x; (void)y; (void)width; (void)height; (void)format; (void)type; (void)pixels; }
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width,
                           GLsizei height)
{
    struct renderbuffer_record *r = renderbuffer_record(s_bound_renderbuffer);

    (void)target;
    if (r == NULL) {
        return;
    }
    r->valid = 1;
    r->format = internalformat;
    r->w = width;
    r->h = height;
}
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{ (void)x; (void)y; (void)width; (void)height; }
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                  GLsizei height, GLint border, GLenum format, GLenum type,
                  const void *pixels)
{
    (void)level;
    (void)internalformat;
    (void)border;
    (void)type;
    texture_image(bound_texture(target), target, format, width, height, 1, pixels);
}
void glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                  GLsizei height, GLsizei depth, GLint border, GLenum format,
                  GLenum type, const void *pixels)
{
    (void)level;
    (void)internalformat;
    (void)border;
    (void)type;
    texture_image(bound_texture(target), target, format, width, height, depth, pixels);
}
void glTexParameteri(GLenum target, GLenum pname, GLint param)
{ (void)target; (void)pname; (void)param; }
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const void *pixels)
{
    (void)level;
    (void)type;
    texture_subimage(bound_texture(target), format, xoffset, yoffset, width, height, pixels);
}
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                           GLsizei stride, const void *pointer)
{
    struct vao_record *v = vao_record(s_gl.current_vao);

    (void)normalized;
    if (v == NULL || index >= MAX_ATTRS) {
        return;
    }
    v->attrib[index].size = size;
    v->attrib[index].type = type;
    v->attrib[index].stride = stride;
    v->attrib[index].offset = (GLintptr)pointer;
    v->attrib[index].buffer = s_array_buffer;
}
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    s_viewport[0] = x;
    s_viewport[1] = y;
    s_viewport[2] = width;
    s_viewport[3] = height;
}

void glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f,
                       GLint g, GLint h, GLbitfield mask, GLenum filter)
{
    s_last_blit.valid = 1;
    s_last_blit.read_fbo = s_read_fbo;
    s_last_blit.draw_fbo = s_draw_fbo;
    s_last_blit.sx0 = a;
    s_last_blit.sy0 = b;
    s_last_blit.sx1 = c;
    s_last_blit.sy1 = d;
    s_last_blit.dx0 = e;
    s_last_blit.dy0 = f;
    s_last_blit.dx1 = g;
    s_last_blit.dy1 = h;
    s_last_blit.mask = mask;
    s_last_blit.filter = filter;
}

void glGetIntegerv(GLenum pname, GLint *data)
{
    *data = pname == GL_NUM_EXTENSIONS ? s_extension_count : 0;
}

const GLubyte *glGetStringi(GLenum name, GLuint index)
{
    if (name != GL_EXTENSIONS || index >= (GLuint)s_extension_count) {
        return NULL;
    }
    return (const GLubyte *)s_extensions[index];
}

/* ------------------------------------------------------------- helpers */

static void check_draw_depth(int enabled, GLenum func, GLboolean mask)
{
    CHECK(s_gl.last_draw.depth_enabled == enabled);
    CHECK(s_gl.last_draw.depth_func == func);
    CHECK(s_gl.last_draw.depth_mask == mask);
}

static void check_draw_cull(int enabled, GLenum face, GLenum front)
{
    CHECK(s_gl.last_draw.cull_enabled == enabled);
    if (enabled) {
        CHECK(s_gl.last_draw.cull_face == face);
        CHECK(s_gl.last_draw.front_face == front);
    }
}

static void check_draw_poly(int enabled, GLfloat factor, GLfloat units)
{
    CHECK(s_gl.last_draw.poly_enabled == enabled);
    if (enabled) {
        CHECK(s_gl.last_draw.poly_factor == factor);
        CHECK(s_gl.last_draw.poly_units == units);
    }
}

static void check_uniform2f(int id, GLfloat a, GLfloat b)
{
    struct uniform_value *u = &s_gl.last_draw.uniforms[id];

    CHECK(u->valid && u->kind == 2);
    CHECK(u->f[0] == a && u->f[1] == b);
}

static void check_uniform1i(int id, GLint a)
{
    struct uniform_value *u = &s_gl.last_draw.uniforms[id];

    CHECK(u->valid && u->kind == 1);
    CHECK(u->i[0] == a);
}

static void check_uniform4f(int id, GLfloat a, GLfloat b, GLfloat c, GLfloat d)
{
    struct uniform_value *u = &s_gl.last_draw.uniforms[id];

    CHECK(u->valid && u->kind == 4);
    CHECK(u->f[0] == a && u->f[1] == b && u->f[2] == c && u->f[3] == d);
}

static void check_same_draw(const struct draw_snapshot *a, const struct draw_snapshot *b)
{
    int i, u;

    CHECK(a->program == b->program);
    CHECK(a->vao == b->vao);
    CHECK(a->array_buffer == b->array_buffer);
    CHECK(a->depth_enabled == b->depth_enabled);
    CHECK(a->depth_func == b->depth_func);
    CHECK(a->depth_mask == b->depth_mask);
    CHECK(a->cull_enabled == b->cull_enabled);
    CHECK(a->cull_face == b->cull_face);
    CHECK(a->front_face == b->front_face);
    CHECK(a->poly_enabled == b->poly_enabled);
    CHECK(a->poly_factor == b->poly_factor);
    CHECK(a->poly_units == b->poly_units);
    CHECK(a->active_texture == b->active_texture);
    for (u = 0; u < 4; u++) {
        CHECK(a->tex2d[u] == b->tex2d[u]);
        CHECK(a->tex_array[u] == b->tex_array[u]);
    }
    for (i = 0; i < UNIFORM_COUNT; i++) {
        CHECK(memcmp(&a->uniforms[i], &b->uniforms[i], sizeof(a->uniforms[i])) == 0);
    }
    for (i = 0; i < MAX_ATTRS; i++) {
        CHECK(a->attrs[i].enabled == b->attrs[i].enabled);
        CHECK(a->attrs[i].size == b->attrs[i].size);
        CHECK(a->attrs[i].stride == b->attrs[i].stride);
        CHECK(a->attrs[i].offset == b->attrs[i].offset);
        CHECK(a->attrs[i].buffer == b->attrs[i].buffer);
        CHECK(a->attrs[i].bytes == b->attrs[i].bytes);
        CHECK(memcmp(a->attrs[i].data, b->attrs[i].data, a->attrs[i].bytes) == 0);
    }
    CHECK(a->mode == b->mode);
    CHECK(a->first == b->first);
    CHECK(a->count == b->count);
}

/* --------------------------------------------------------------- tests */

static void test_first_and_repeated(void)
{
    unsigned tex;
    struct draw_snapshot first;

    fake_reset();
    CHECK(rnd_init() != 0);
    tex = rnd_texture(RND_R8, 8, 8, NULL, 0);
    calls_clear();

    /* First public state calls after init may not assume GL defaults. */
    rnd_use(RND_CANVAS);
    rnd_depth(RND_DEPTH_LESS, 1);
    rnd_cull(RND_CULL_CCW);
    rnd_offset(1);
    rnd_texture_unit(0, tex);
    CHECK(s_calls.use_program == 1);
    CHECK(s_calls.bind_vao == 1);
    CHECK(s_calls.enable_depth == 1);
    CHECK(s_calls.depth_func == 1);
    CHECK(s_calls.depth_mask == 1);
    CHECK(s_calls.enable_cull == 1);
    CHECK(s_calls.cull_face == 1);
    CHECK(s_calls.front_face == 1);
    CHECK(s_calls.enable_poly == 1);
    CHECK(s_calls.polygon_offset == 1);
    CHECK(s_calls.active_texture == 1);
    CHECK(s_calls.bind_tex2d == 1);
    rnd_draw(RND_TRIANGLES, 0, 0);
    first = s_gl.last_draw;
    CHECK(s_calls.draws == 1);
    check_draw_depth(1, GL_LESS, GL_TRUE);
    check_draw_cull(1, GL_BACK, GL_CW);
    check_draw_poly(1, -1.0f, -4.0f);
    CHECK(s_gl.last_draw.active_texture == GL_TEXTURE0);
    CHECK(s_gl.last_draw.tex2d[0] == tex);
    CHECK(s_gl.last_draw.program == s_next_program - RND_PROGRAMS);

    calls_clear();
    rnd_use(RND_CANVAS);
    rnd_depth(RND_DEPTH_LESS, 1);
    rnd_cull(RND_CULL_CCW);
    rnd_offset(1);
    rnd_texture_unit(0, tex);
    CHECK(s_calls.use_program == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.bind_vao == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.enable_depth == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.depth_func == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.depth_mask == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.enable_cull == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.cull_face == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.front_face == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.enable_poly == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.polygon_offset == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.active_texture == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.bind_tex2d == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    rnd_draw(RND_STRIP, 3, 7);
    CHECK(s_calls.draws == 1);
    CHECK(memcmp(&first, &s_gl.last_draw, sizeof(first)) != 0);
    first.mode = GL_TRIANGLE_STRIP;
    first.first = 3;
    first.count = 7;
    CHECK(memcmp(&first, &s_gl.last_draw, sizeof(first)) == 0);

    /* Changed values are emitted, and unchanged companions are not. */
    calls_clear();
    rnd_depth(RND_DEPTH_LEQUAL, 1);
    CHECK(s_calls.enable_depth == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.depth_func == 1);
    CHECK(s_calls.depth_mask == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    rnd_depth(RND_DEPTH_LEQUAL, 0);
    CHECK(s_calls.depth_func == (TEST_EXPECT_STATE_CACHE ? 1 : 2));
    CHECK(s_calls.depth_mask == (TEST_EXPECT_STATE_CACHE ? 1 : 2));
    rnd_depth(RND_DEPTH_OFF, 0);
    CHECK(s_calls.disable_depth == 1);
    CHECK(s_calls.depth_func == (TEST_EXPECT_STATE_CACHE ? 1 : 2));
    rnd_draw(RND_TRIANGLES, 0, 0);
    check_draw_depth(0, GL_LEQUAL, GL_FALSE);

    /* Re-enabling keeps the remembered function. */
    calls_clear();
    rnd_depth(RND_DEPTH_LESS, 1);
    CHECK(s_calls.enable_depth == 1);
    CHECK(s_calls.depth_func == 1);
    CHECK(s_calls.depth_mask == 1);
    rnd_depth(RND_DEPTH_LESS, 1);
    CHECK(s_calls.enable_depth == (TEST_EXPECT_STATE_CACHE ? 1 : 2));
    CHECK(s_calls.depth_func == (TEST_EXPECT_STATE_CACHE ? 1 : 2));
    CHECK(s_calls.depth_mask == (TEST_EXPECT_STATE_CACHE ? 1 : 2));

    /* Cull only emits the fields that changed. */
    calls_clear();
    rnd_cull(RND_CULL_CW);
    CHECK(s_calls.enable_cull == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.cull_face == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.front_face == 1);
    rnd_cull(RND_CULL_OFF);
    CHECK(s_calls.disable_cull == 1);
    rnd_cull(RND_CULL_OFF);
    CHECK(s_calls.disable_cull == (TEST_EXPECT_STATE_CACHE ? 1 : 2));
    rnd_cull(RND_CULL_CCW);
    CHECK(s_calls.enable_cull == (TEST_EXPECT_STATE_CACHE ? 1 : 2));
    CHECK(s_calls.front_face == 2);
    rnd_draw(RND_TRIANGLES, 0, 0);
    check_draw_cull(1, GL_BACK, GL_CW);

    calls_clear();
    rnd_offset(0);
    CHECK(s_calls.disable_poly == 1);
    rnd_offset(0);
    CHECK(s_calls.disable_poly == (TEST_EXPECT_STATE_CACHE ? 1 : 2));
    rnd_draw(RND_TRIANGLES, 0, 0);
    check_draw_poly(0, 0.0f, 0.0f);

    rnd_use(RND_CANVAS);
    rnd_draw(RND_TRIANGLES, 0, 0);
    check_draw_depth(1, GL_LESS, GL_TRUE);
    check_draw_cull(1, GL_BACK, GL_CW);
    check_draw_poly(0, 0.0f, 0.0f);
    rnd_shutdown();
}

static void test_textures(void)
{
    unsigned a, b, pages, reused, padded;
    unsigned char pixels[8 * 10];
    int next;

    memset(pixels, 0, sizeof(pixels));
    fake_reset();
    CHECK(rnd_init() != 0);
    a = rnd_texture(RND_R8, 8, 8, NULL, 2);
    b = rnd_texture(RND_RG8, 8, 8, NULL, 0);
    pages = rnd_pages(NULL, 3);
    CHECK(a != 0 && b != 0 && pages != 0);
    calls_clear();

    rnd_texture_unit(0, a);
    CHECK(s_calls.active_texture == 1);
    CHECK(s_calls.bind_tex2d == 1);
    rnd_texture_unit(0, a);
    CHECK(s_calls.active_texture == REPEATED_ONCE);
    CHECK(s_calls.bind_tex2d == REPEATED_ONCE);

    rnd_texture_unit(1, b);
    CHECK(s_calls.active_texture == (TEST_EXPECT_STATE_CACHE ? 2 : 3));
    CHECK(s_calls.bind_tex2d == (TEST_EXPECT_STATE_CACHE ? 2 : 3));
    rnd_texture_unit(0, a);
    CHECK(s_calls.active_texture == (TEST_EXPECT_STATE_CACHE ? 3 : 4));
    CHECK(s_calls.bind_tex2d == (TEST_EXPECT_STATE_CACHE ? 2 : 4));

    rnd_texture_unit(0, pages);
    CHECK(s_calls.bind_tex_array == 1);
    CHECK(s_calls.bind_tex2d == (TEST_EXPECT_STATE_CACHE ? 2 : 4));
    rnd_draw(RND_TRIANGLES, 0, 0);
    CHECK(s_gl.last_draw.active_texture == GL_TEXTURE0);
    CHECK(s_gl.last_draw.tex2d[0] == a);
    CHECK(s_gl.last_draw.tex_array[0] == pages);
    CHECK(s_gl.last_draw.tex2d[1] == b);

    calls_clear();
    rnd_texture_unit(0, 0);
    CHECK(s_calls.bind_tex2d == 1);
    CHECK(s_calls.bind_tex_array == 1);
    rnd_texture_unit(0, 0);
    CHECK(s_calls.bind_tex2d == REPEATED_ONCE);
    CHECK(s_calls.bind_tex_array == REPEATED_ONCE);
    rnd_draw(RND_TRIANGLES, 0, 0);
    CHECK(s_gl.last_draw.tex2d[0] == 0);
    CHECK(s_gl.last_draw.tex_array[0] == 0);
    CHECK(s_gl.last_draw.tex2d[1] == b);

    /* Units beyond the fixed cache are passed through unchanged. */
    calls_clear();
    rnd_texture_unit(32, a);
    rnd_texture_unit(32, a);
    rnd_texture_unit(32, pages);
    rnd_texture_unit(32, pages);
    CHECK(s_calls.active_texture == 4);
    CHECK(s_calls.bind_tex2d == 2);
    CHECK(s_calls.bind_tex_array == 2);

    /* Work-unit uploads update the same cache. */
    calls_clear();
    rnd_texture_set(a, 8, 8, NULL, 0);
    CHECK(s_calls.active_texture == 1);
    CHECK(s_calls.bind_tex2d == 1);
    rnd_texture_set(a, 8, 8, NULL, 0);
    CHECK(s_calls.active_texture == REPEATED_ONCE);
    CHECK(s_calls.bind_tex2d == REPEATED_ONCE);
    rnd_texture_set(a, 4, 4, NULL, 0);
    rnd_copy(b, 0, 0, 2, 2);
    CHECK(s_calls.active_texture == (TEST_EXPECT_STATE_CACHE ? 1 : 4));
    CHECK(s_calls.bind_tex2d == (TEST_EXPECT_STATE_CACHE ? 2 : 4));
    rnd_copy(b, 0, 0, 2, 2);
    CHECK(s_calls.active_texture == (TEST_EXPECT_STATE_CACHE ? 1 : 5));
    CHECK(s_calls.bind_tex2d == (TEST_EXPECT_STATE_CACHE ? 2 : 5));

    /* Freeing a bound name drops every binding, then the name may return. */
    calls_clear();
    rnd_texture_free(a);
    rnd_texture_unit(0, 0);
    CHECK(s_calls.bind_tex2d == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.bind_tex_array == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    next = (int)s_next_texture;
    s_next_texture = a;
    reused = rnd_texture(RND_R8, 8, 8, NULL, 0);
    CHECK(reused == a);
    s_next_texture = (GLuint)next;
    calls_clear();
    rnd_texture_unit(0, reused);
    CHECK(s_calls.active_texture == 1);
    CHECK(s_calls.bind_tex2d == 1);
    rnd_draw(RND_TRIANGLES, 0, 0);
    CHECK(s_gl.last_draw.tex2d[0] == reused);

    /* A creation while unit 7 is already current still binds the new name. */
    rnd_copy(b, 0, 0, 2, 2);
    calls_clear();
    a = rnd_texture(RND_R8, 8, 8, NULL, 0);
    CHECK(s_calls.active_texture == (TEST_EXPECT_STATE_CACHE ? 0 : 1));
    CHECK(s_calls.bind_tex2d == 1);
    source_expect(pixels, sizeof(pixels));
    padded = rnd_texture(RND_R8, 8, 8, pixels, 10);
    rnd_texture_free(0);
    rnd_texture_free(padded);
    rnd_texture_free(a);
    rnd_texture_free(b);
    rnd_texture_free(reused);
    rnd_texture_free(pages);
    rnd_shutdown();
}

static void test_uniforms(void)
{
    float lsize[2] = { 64.0f, 32.0f };
    float off[2] = { 1.0f, 2.0f };
    float mode[1] = { 1.7f };
    float rect[4] = { 0.0f, 1.0f, 2.0f, 3.0f };
    float matrix[16];
    float other_matrix[16];
    float use_table[1] = { 1.0f };
    float size[2] = { 8.0f, 8.0f };
    float mode_same_int[1] = { 1.2f };
    int i;

    for (i = 0; i < 16; i++) {
        matrix[i] = (float)i / 10.0f;
        other_matrix[i] = (float)i / 20.0f;
    }
    fake_reset();
    CHECK(rnd_init() != 0);
    rnd_use(RND_CANVAS);
    calls_clear();

    rnd_uniform(RND_U_LSIZE, lsize);
    rnd_uniform(RND_U_OFF, off);
    rnd_uniform(RND_U_MODE, mode);
    rnd_uniform(RND_U_RECT, rect);
    rnd_uniform(RND_U_MATRIX, matrix);
    rnd_uniform(RND_U_USE_TABLE, use_table);
    rnd_uniform(RND_U_SIZE, size);
    CHECK(s_calls.uniform1i == 2);
    CHECK(s_calls.uniform2f == 3);
    CHECK(s_calls.uniform4f == 1);
    CHECK(s_calls.uniform_matrix == 1);
    rnd_uniform(RND_U_LSIZE, lsize);
    rnd_uniform(RND_U_OFF, off);
    rnd_uniform(RND_U_MODE, mode_same_int);
    rnd_uniform(RND_U_RECT, rect);
    rnd_uniform(RND_U_MATRIX, matrix);
    rnd_uniform(RND_U_USE_TABLE, use_table);
    rnd_uniform(RND_U_SIZE, size);
    CHECK(s_calls.uniform1i == (TEST_EXPECT_STATE_CACHE ? 2 : 4));
    CHECK(s_calls.uniform2f == (TEST_EXPECT_STATE_CACHE ? 3 : 6));
    CHECK(s_calls.uniform4f == (TEST_EXPECT_STATE_CACHE ? 1 : 2));
    CHECK(s_calls.uniform_matrix == (TEST_EXPECT_STATE_CACHE ? 1 : 2));
    rnd_draw(RND_TRIANGLES, 0, 0);
    check_uniform2f(RND_U_LSIZE, 64.0f, 32.0f);
    check_uniform2f(RND_U_OFF, 1.0f, 2.0f);
    check_uniform1i(RND_U_MODE, 1);
    check_uniform4f(RND_U_RECT, 0.0f, 1.0f, 2.0f, 3.0f);
    CHECK(s_gl.last_draw.uniforms[RND_U_MATRIX].valid);
    CHECK(s_gl.last_draw.uniforms[RND_U_MATRIX].kind == 16);
    CHECK(memcmp(s_gl.last_draw.uniforms[RND_U_MATRIX].f, matrix,
                 sizeof(matrix)) == 0);
    check_uniform1i(RND_U_USE_TABLE, 1);
    check_uniform2f(RND_U_SIZE, 8.0f, 8.0f);

    /* Uniform values belong to programs, not just ids. */
    calls_clear();
    rnd_use(RND_MESH);
    rnd_uniform(RND_U_MATRIX, other_matrix);
    CHECK(s_calls.uniform_matrix == 1);
    rnd_use(RND_CANVAS);
    rnd_uniform(RND_U_MATRIX, matrix);
    CHECK(s_calls.uniform_matrix == (TEST_EXPECT_STATE_CACHE ? 1 : 2));
    matrix[0] = 9.0f;
    rnd_uniform(RND_U_MATRIX, matrix);
    CHECK(s_calls.uniform_matrix == (TEST_EXPECT_STATE_CACHE ? 2 : 3));
    lsize[0] = 128.0f;
    rect[2] = 6.0f;
    rnd_uniform(RND_U_LSIZE, lsize);
    rnd_uniform(RND_U_RECT, rect);
    rnd_draw(RND_TRIANGLES, 0, 0);
    CHECK(memcmp(s_gl.last_draw.uniforms[RND_U_MATRIX].f, matrix,
                 sizeof(matrix)) == 0);
    check_uniform2f(RND_U_LSIZE, 128.0f, 32.0f);
    check_uniform4f(RND_U_RECT, 0.0f, 1.0f, 6.0f, 3.0f);
    mode[0] = 0.0f;
    calls_clear();
    rnd_uniform(RND_U_MODE, mode);
    CHECK(s_calls.uniform1i == 1);
    rnd_draw(RND_TRIANGLES, 0, 0);
    check_uniform1i(RND_U_MODE, 0);
    rnd_shutdown();
}

static void test_uncached_unit_restores_cache(void)
{
    unsigned a, b, c;

    fake_reset();
    CHECK(rnd_init() != 0);
    a = rnd_texture(RND_R8, 8, 8, NULL, 0);
    b = rnd_texture(RND_R8, 8, 8, NULL, 0);
    c = rnd_texture(RND_R8, 8, 8, NULL, 0);
    calls_clear();

    /* Unit 32 is legal but outside the fixed cache. Going through it and
     * back to unit 0 must leave both units with the requested textures. */
    rnd_texture_unit(0, a);
    rnd_texture_unit(32, b);
    rnd_texture_unit(0, c);
    rnd_draw(RND_TRIANGLES, 0, 0);
    CHECK(s_gl.last_draw.active_texture == GL_TEXTURE0);
    CHECK(s_gl.last_draw.tex2d[0] == c);
    CHECK(s_gl.last_draw.tex2d[32] == b);

    /* The same holds when the uncached unit stays current at the draw. */
    rnd_texture_unit(0, a);
    rnd_texture_unit(32, c);
    rnd_draw(RND_TRIANGLES, 0, 0);
    CHECK(s_gl.last_draw.active_texture == GL_TEXTURE0 + 32);
    CHECK(s_gl.last_draw.tex2d[0] == a);
    CHECK(s_gl.last_draw.tex2d[32] == c);

    rnd_texture_free(a);
    rnd_texture_free(b);
    rnd_texture_free(c);
    rnd_shutdown();
}

static void test_upload_records(void)
{
    unsigned char r8[12], r8b[12], small[4], rg[24], rg_tight[12];
    unsigned a, b, pages, reused;
    struct texture_record *r;
    int i, next;

    for (i = 0; i < (int)sizeof(r8); i++) {
        r8[i] = (unsigned char)(i + 1);
        r8b[i] = (unsigned char)(i + 101);
    }
    for (i = 0; i < (int)sizeof(small); i++) {
        small[i] = (unsigned char)(i + 201);
    }
    for (i = 0; i < (int)sizeof(rg); i++) {
        rg[i] = (unsigned char)(i + 31);
    }
    memcpy(rg_tight, rg, 6);
    memcpy(rg_tight + 6, rg + 12, 6);

    fake_reset();
    CHECK(rnd_init() != 0);
    source_expect(r8, sizeof(r8));
    a = rnd_texture(RND_R8, 4, 3, r8, 4);
    r = texture_record(a);
    CHECK(r != NULL && r->valid);
    CHECK(r->target == GL_TEXTURE_2D);
    CHECK(r->w == 4 && r->h == 3 && r->bytes == 12 && !r->truncated);
    CHECK(memcmp(r->data, r8, sizeof(r8)) == 0);

    rnd_texture_unit(0, a);
    source_expect(rg, sizeof(rg));
    b = rnd_texture(RND_RG8, 3, 2, rg, 6);
    CHECK(s_gl.tex2d[0] == a);
    r = texture_record(b);
    CHECK(r != NULL && r->valid);
    CHECK(r->w == 3 && r->h == 2 && r->bytes == 12);
    CHECK(memcmp(r->data, rg_tight, sizeof(rg_tight)) == 0);

    source_expect(r8b, sizeof(r8b));
    rnd_texture_set(a, 4, 3, r8b, 4);
    r = texture_record(a);
    CHECK(r != NULL && r->w == 4 && r->h == 3 && r->bytes == 12);
    CHECK(memcmp(r->data, r8b, sizeof(r8b)) == 0);
    source_expect(small, sizeof(small));
    rnd_texture_set(a, 2, 2, small, 2);
    CHECK(r != NULL && r->w == 2 && r->h == 2 && r->bytes == 4);
    CHECK(memcmp(r->data, small, sizeof(small)) == 0);
    CHECK(s_gl.tex2d[0] == a);

    pages = rnd_pages(NULL, 3);
    r = texture_record(pages);
    CHECK(r != NULL && r->valid);
    CHECK(r->target == GL_TEXTURE_2D_ARRAY);
    CHECK(r->w == 256 && r->h == 256 && r->depth == 3);
    CHECK(r->bytes == (size_t)256 * 256 * 3);
    CHECK(s_gl.tex2d[0] == a);

    rnd_texture_free(a);
    CHECK(!texture_record(a)->valid);
    next = (int)s_next_texture;
    s_next_texture = a;
    source_expect(small, sizeof(small));
    reused = rnd_texture(RND_R8, 2, 2, small, 2);
    s_next_texture = (GLuint)next;
    CHECK(reused == a);
    r = texture_record(reused);
    CHECK(r != NULL && r->valid && r->bytes == 4);
    CHECK(memcmp(r->data, small, sizeof(small)) == 0);
    CHECK(s_gl.tex2d[0] == 0);

    rnd_texture_free(b);
    rnd_texture_free(pages);
    rnd_texture_free(reused);
    rnd_shutdown();
}

static void test_draw_data_after_state_changes(void)
{
    unsigned char src_px[16], dst_px[16], extra_px[4];
    float verts1[8] = { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f };
    float verts2[8] = { 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f };
    float dyn[27], mesh[24], more[20];
    float lsize[2] = { 4.0f, 4.0f };
    float mode[1] = { 0.0f };
    unsigned src, dst, extra;
    struct draw_snapshot baseline, while_working;
    int i;

    for (i = 0; i < 16; i++) {
        src_px[i] = (unsigned char)(i + 1);
        dst_px[i] = (unsigned char)(i + 81);
    }
    for (i = 0; i < 4; i++) {
        extra_px[i] = (unsigned char)(i + 61);
    }
    for (i = 0; i < 27; i++) {
        dyn[i] = (float)(i + 21);
    }
    for (i = 0; i < 24; i++) {
        mesh[i] = (float)(i + 41);
    }
    for (i = 0; i < 20; i++) {
        more[i] = (float)(i + 301);
    }

    fake_reset();
    CHECK(rnd_init() != 0);
    source_expect(src_px, sizeof(src_px));
    src = rnd_texture(RND_R8, 4, 4, src_px, 4);
    source_expect(dst_px, sizeof(dst_px));
    dst = rnd_texture(RND_R8, 4, 4, dst_px, 4);

    rnd_use(RND_CANVAS);
    rnd_texture_unit(0, src);
    rnd_uniform(RND_U_LSIZE, lsize);
    rnd_uniform(RND_U_MODE, mode);
    rnd_stream(verts1, 2);
    rnd_draw(RND_TRIANGLES, 0, 2);
    baseline = s_gl.last_draw;
    CHECK(baseline.attrs[0].bytes == 64);
    CHECK(memcmp(baseline.attrs[0].data, verts1, sizeof(verts1)) == 0);
    CHECK(baseline.tex2d[0] == src);
    CHECK(baseline.active_texture == GL_TEXTURE0);

    /* Work-unit uploads and copies must leave the sampler unit alone. */
    source_expect(src_px, sizeof(src_px));
    rnd_texture_set(dst, 4, 4, src_px, 4);
    rnd_copy(dst, 0, 0, 2, 2);
    source_expect(extra_px, sizeof(extra_px));
    extra = rnd_texture(RND_R8, 2, 2, extra_px, 2);
    rnd_draw(RND_TRIANGLES, 0, 2);
    while_working = s_gl.last_draw;
    CHECK(while_working.active_texture == GL_TEXTURE0 + 7);
    CHECK(while_working.tex2d[0] == src);
    CHECK(while_working.attrs[0].buffer == baseline.attrs[0].buffer);
    CHECK(memcmp(while_working.attrs[0].data, verts1, sizeof(verts1)) == 0);

    /* Other programs and VAOs write their own buffers, not CANVAS's. */
    rnd_use(RND_DYN);
    rnd_texture_unit(0, dst);
    rnd_stream(dyn, 3);
    rnd_draw(RND_TRIANGLES, 0, 3);
    CHECK(s_gl.last_draw.program != baseline.program);
    CHECK(s_gl.last_draw.attrs[0].bytes == 216);
    CHECK(memcmp(s_gl.last_draw.attrs[0].data, dyn, sizeof(dyn)) == 0);

    rnd_use(RND_MESH);
    rnd_mesh_store(3);
    rnd_mesh_write(0, 3, mesh);
    rnd_draw_mesh(0, 3);
    CHECK(s_gl.last_draw.attrs[0].bytes == 96);
    CHECK(memcmp(s_gl.last_draw.attrs[0].data, mesh, sizeof(mesh)) == 0);

    rnd_use(RND_CANVAS);
    rnd_texture_unit(0, src);
    rnd_stream(verts1, 2);
    rnd_draw(RND_TRIANGLES, 0, 2);
    check_same_draw(&baseline, &s_gl.last_draw);

    /* New stream data replaces the bytes the next draw sees. */
    rnd_stream(verts2, 2);
    rnd_draw(RND_TRIANGLES, 0, 2);
    CHECK(memcmp(s_gl.last_draw.attrs[0].data, verts2, sizeof(verts2)) == 0);
    CHECK(memcmp(s_gl.last_draw.attrs[0].data, verts1, sizeof(verts1)) != 0);

    /* Growing past the initial VBO capacity reallocates it with the new data. */
    rnd_stream(more, 5);
    rnd_draw(RND_TRIANGLES, 0, 5);
    CHECK(s_gl.last_draw.attrs[0].bytes == 80);
    CHECK(memcmp(s_gl.last_draw.attrs[0].data, more, sizeof(more)) == 0);

    rnd_texture_free(src);
    rnd_texture_free(dst);
    rnd_texture_free(extra);
    rnd_shutdown();
}

static void test_mesh_data(void)
{
    unsigned char pixels[4];
    float mesh[24], other[8], canvas[8];
    unsigned tex;
    struct draw_snapshot first;
    struct buffer_record *b;
    int i;

    for (i = 0; i < 4; i++) {
        pixels[i] = (unsigned char)(i + 1);
    }
    for (i = 0; i < 24; i++) {
        mesh[i] = (float)(i + 1);
    }
    for (i = 0; i < 8; i++) {
        other[i] = (float)(i + 101);
        canvas[i] = (float)(i + 201);
    }

    fake_reset();
    CHECK(rnd_init() != 0);
    rnd_use(RND_MESH);
    rnd_mesh_store(3);
    b = buffer_record(s_array_buffer);
    CHECK(b != NULL && b->valid && b->size == 96 && !b->truncated);
    rnd_mesh_write(0, 3, mesh);
    CHECK(memcmp(b->data, mesh, sizeof(mesh)) == 0);
    rnd_draw_mesh(0, 3);
    first = s_gl.last_draw;
    CHECK(first.program != 0 && first.vao != 0);
    CHECK(first.array_buffer == s_array_buffer);
    CHECK(first.attrs[0].enabled);
    CHECK(first.attrs[0].size == 3 && first.attrs[0].stride == 32);
    CHECK(first.attrs[0].offset == 0 && first.attrs[0].buffer == s_array_buffer);
    CHECK(first.attrs[0].bytes == 96);
    CHECK(first.attrs[1].size == 2 && first.attrs[1].offset == 12);
    CHECK(first.attrs[2].size == 3 && first.attrs[2].offset == 20);
    CHECK(memcmp(first.attrs[0].data, mesh, sizeof(mesh)) == 0);

    rnd_mesh_write(1, 1, other);
    rnd_draw_mesh(0, 3);
    CHECK(memcmp(s_gl.last_draw.attrs[0].data, mesh, 32) == 0);
    CHECK(memcmp(s_gl.last_draw.attrs[0].data + 32, other, sizeof(other)) == 0);

    rnd_use(RND_CANVAS);
    source_expect(pixels, sizeof(pixels));
    tex = rnd_texture(RND_R8, 2, 2, pixels, 2);
    rnd_texture_unit(0, tex);
    rnd_stream(canvas, 2);
    rnd_draw(RND_TRIANGLES, 0, 2);
    CHECK(s_gl.last_draw.program != first.program);
    CHECK(s_gl.last_draw.array_buffer != first.array_buffer);
    CHECK(memcmp(s_gl.last_draw.attrs[0].data, canvas, sizeof(canvas)) == 0);

    rnd_use(RND_MESH);
    rnd_draw_mesh(0, 3);
    CHECK(s_gl.last_draw.program == first.program);
    CHECK(s_gl.last_draw.vao == first.vao);
    CHECK(s_gl.last_draw.array_buffer != first.array_buffer);
    CHECK(s_gl.last_draw.attrs[0].buffer == first.attrs[0].buffer);
    CHECK(memcmp(s_gl.last_draw.attrs[0].data, mesh, 32) == 0);
    CHECK(memcmp(s_gl.last_draw.attrs[0].data + 32, other, sizeof(other)) == 0);
    CHECK(memcmp(s_gl.last_draw.attrs[0].data + 64, mesh + 16, 32) == 0);

    rnd_texture_free(tex);
    rnd_shutdown();
}

static void test_targets_blits_copies(void)
{
    unsigned char pixels[8 * 8];
    unsigned c1, c2, t1, t2;
    struct framebuffer_record *f;
    struct renderbuffer_record *rb;
    int w, h, i;

    for (i = 0; i < (int)sizeof(pixels); i++) {
        pixels[i] = (unsigned char)(i + 1);
    }
    fake_reset();
    CHECK(rnd_init() != 0);
    source_expect(pixels, sizeof(pixels));
    c1 = rnd_texture(RND_R8, 8, 8, pixels, 8);
    source_expect(pixels, sizeof(pixels));
    c2 = rnd_texture(RND_R8, 4, 4, pixels, 4);
    rnd_texture_size(c1, &w, &h);
    CHECK(w == 8 && h == 8);

    t1 = rnd_target(c1, 1);
    f = framebuffer_record(t1);
    CHECK(f != NULL && f->valid && f->color == c1 && f->depth != 0);
    rb = renderbuffer_record(f->depth);
    CHECK(rb != NULL && rb->valid);
    CHECK(rb->format == GL_DEPTH_COMPONENT24 && rb->w == 8 && rb->h == 8);
    CHECK(s_read_fbo == 0 && s_draw_fbo == 0);

    rnd_bind(t1);
    CHECK(s_read_fbo == t1 && s_draw_fbo == t1);
    CHECK(s_viewport[2] == 8 && s_viewport[3] == 8);
    rnd_copy(c1, 1, 2, 3, 4);
    CHECK(s_last_copy.valid && s_last_copy.texture == c1);
    CHECK(s_last_copy.read_fbo == t1 && s_last_copy.unit == 7);
    CHECK(s_last_copy.target == GL_TEXTURE_2D);
    CHECK(s_last_copy.xoffset == 1 && s_last_copy.yoffset == 2);
    CHECK(s_last_copy.x == 1 && s_last_copy.y == 2);
    CHECK(s_last_copy.w == 3 && s_last_copy.h == 4);

    rnd_bind_screen(20, 10);
    CHECK(s_read_fbo == 0 && s_draw_fbo == 0);
    CHECK(s_viewport[2] == 20 && s_viewport[3] == 10);
    t2 = rnd_target(c2, 0);
    f = framebuffer_record(t2);
    CHECK(f != NULL && f->valid && f->color == c2 && f->depth == 0);
    CHECK(s_read_fbo == 0 && s_draw_fbo == 0);

    rnd_bind(t1);
    rnd_blit(t1, t2, 8, 8, 4, 4);
    CHECK(s_last_blit.valid && s_last_blit.read_fbo == t1 && s_last_blit.draw_fbo == t2);
    CHECK(s_last_blit.sx0 == 0 && s_last_blit.sy0 == 0);
    CHECK(s_last_blit.sx1 == 8 && s_last_blit.sy1 == 8);
    CHECK(s_last_blit.dx1 == 4 && s_last_blit.dy1 == 4);
    CHECK(s_last_blit.mask == GL_COLOR_BUFFER_BIT && s_last_blit.filter == GL_NEAREST);
    CHECK(s_read_fbo == t1 && s_draw_fbo == t1);

    rnd_target_free(t2);
    CHECK(!framebuffer_record(t2)->valid);
    rnd_target_free(t1);
    CHECK(!framebuffer_record(t1)->valid);
    CHECK(s_read_fbo == 0 && s_draw_fbo == 0);

    rnd_texture_free(c1);
    rnd_texture_free(c2);
    rnd_shutdown();
}

static void test_lifecycle(void)
{
    fake_reset();
    s_fail_shader = 1;
    CHECK(rnd_init() == 0);
    s_fail_shader = 0;
    fake_reset();
    s_fail_fragment = 1;
    CHECK(rnd_init() == 0);
    s_fail_fragment = 0;
    fake_reset();
    s_fail_link = 1;
    CHECK(rnd_init() == 0);
    s_fail_link = 0;

    /* A failed attempt leaves nothing that lets the next init skip state. */
    CHECK(rnd_init() != 0);
    calls_clear();
    rnd_use(RND_CANVAS);
    rnd_depth(RND_DEPTH_LESS, 1);
    CHECK(s_calls.use_program == 1);
    CHECK(s_calls.bind_vao == 1);
    CHECK(s_calls.enable_depth == 1);
    CHECK(s_calls.depth_func == 1);
    CHECK(s_calls.depth_mask == 1);

    /* Shutdown and re-init cannot expose a stale cache. */
    rnd_shutdown();
    calls_clear();
    CHECK(rnd_init() != 0);
    calls_clear();
    rnd_use(RND_CANVAS);
    rnd_texture_unit(0, 0);
    CHECK(s_calls.use_program == 1);
    CHECK(s_calls.bind_vao == 1);
    CHECK(s_calls.active_texture == 1);
    CHECK(s_calls.bind_tex2d == 1);
    CHECK(s_calls.bind_tex_array == 1);
    rnd_shutdown();
}

static void test_init_fetch_paths(void)
{
#if TEST_HAVE_FETCH_BUILD
    static const char *const fetch_extension[] = { "GL_EXT_shader_framebuffer_fetch" };

    /* The advertised extension selects the fetch program. */
    fake_reset();
    fake_extensions(fetch_extension, 1);
    CHECK(rnd_init() != 0);
    CHECK(rnd_dyn_framebuffer_fetch() == 1);
    calls_clear();
    rnd_use(RND_CANVAS);
    CHECK(s_calls.use_program == 1);
    CHECK(s_calls.bind_vao == 1);
    rnd_shutdown();

    /* A fetch compile failure falls back to the ordinary DYN program. */
    fake_reset();
    fake_extensions(fetch_extension, 1);
    s_fail_fetch_shader = 1;
    CHECK(rnd_init() != 0);
    CHECK(rnd_dyn_framebuffer_fetch() == 0);
    s_fail_fetch_shader = 0;
    calls_clear();
    rnd_use(RND_CANVAS);
    CHECK(s_calls.use_program == 1);
    CHECK(s_calls.bind_vao == 1);
    rnd_shutdown();

    /* When both the fetch and fallback programs fail, init reports failure
     * and the next successful init still emits every first-use call. */
    fake_reset();
    fake_extensions(fetch_extension, 1);
    s_fail_dyn_fragment = 1;
    CHECK(rnd_init() == 0);
    CHECK(rnd_dyn_framebuffer_fetch() == 0);
    s_fail_dyn_fragment = 0;
    CHECK(rnd_init() != 0);
    CHECK(rnd_dyn_framebuffer_fetch() == 1);
    calls_clear();
    rnd_use(RND_CANVAS);
    rnd_depth(RND_DEPTH_LESS, 1);
    CHECK(s_calls.use_program == 1);
    CHECK(s_calls.bind_vao == 1);
    CHECK(s_calls.enable_depth == 1);
    CHECK(s_calls.depth_func == 1);
    CHECK(s_calls.depth_mask == 1);
    rnd_shutdown();
#endif
}

int main(void)
{
    test_first_and_repeated();
    test_textures();
    test_uniforms();
    test_uncached_unit_restores_cache();
    test_upload_records();
    test_draw_data_after_state_changes();
    test_mesh_data();
    test_targets_blits_copies();
    test_lifecycle();
    test_init_fetch_paths();

    if (s_failures != 0) {
        fprintf(stderr, "test_perf_state_cache: %d failures\n", s_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
