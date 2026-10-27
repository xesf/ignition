/*
 * rnd_gl.c - rnd.h on OpenGL 3.2 core (macOS, Linux, Windows) and
 * OpenGL ES 3.0 (the web, Android, iOS); see rnd_gl.h.
 *
 * Textures and targets are GL names: a texture's name, a target's
 * framebuffer. Units 0 to 3 hold what the programs read; making and
 * filling textures goes through unit 7, so it never disturbs them.
 *
 * The shaders are GLSL 1.50. On ES each one's "#version 150" becomes
 * "#version 300 es" with default precisions; ES has no
 * glBindFragDataLocation (a shader's one output goes to attachment 0)
 * and reads pixels back only as RGBA.
 */

#if defined(IGN_GLES)
#if defined(__APPLE__)
#ifndef GLES_SILENCE_DEPRECATION
#define GLES_SILENCE_DEPRECATION
#endif
#include <OpenGLES/ES3/gl.h>
#else
#include <GLES3/gl3.h>
#endif
#elif defined(__APPLE__)
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl3.h>
#else
/* Linux and Windows: Khronos' declarations (third_party/), and every
 * entry point through a pointer that rnd_gl_load fills in. With
 * APIENTRY set, glcorearb.h does not pull in <windows.h>. */
#if defined(_WIN32) && !defined(APIENTRY)
#define APIENTRY __stdcall
#endif
#include "third_party/GL/glcorearb.h"
#define IGN_GL_LOAD 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rnd.h"
#include "rnd_gl.h"
#include "prof.h"

unsigned rnd_gl_screen_fbo;

#if defined(IGN_GL_LOAD)

#define IGN_GL_FUNCS(X) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBINDFRAGDATALOCATIONPROC, glBindFragDataLocation) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
    X(PFNGLCLEARPROC, glClear) \
    X(PFNGLCLEARCOLORPROC, glClearColor) \
    X(PFNGLCLEARDEPTHPROC, glClearDepth) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLCOPYTEXSUBIMAGE2DPROC, glCopyTexSubImage2D) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLCULLFACEPROC, glCullFace) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLDEPTHMASKPROC, glDepthMask) \
    X(PFNGLDISABLEPROC, glDisable) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
    X(PFNGLENABLEPROC, glEnable) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLFRONTFACEPROC, glFrontFace) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset) \
    X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLSCISSORPROC, glScissor) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) \
    X(PFNGLTEXIMAGE3DPROC, glTexImage3D) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLVIEWPORTPROC, glViewport)

#define IGN_GL_POINTER(type, name) static type p_##name;
IGN_GL_FUNCS(IGN_GL_POINTER)
#undef IGN_GL_POINTER

int rnd_gl_load(void *(*get)(const char *name))
{
    void *p;

#define IGN_GL_GET(type, name) \
    p = get(#name); \
    if (p == NULL) { \
        fprintf(stderr, "rnd: OpenGL has no %s\n", #name); \
        return 0; \
    } \
    memcpy(&p_##name, &p, sizeof(p));
    IGN_GL_FUNCS(IGN_GL_GET)
#undef IGN_GL_GET
    return 1;
}

#define glActiveTexture p_glActiveTexture
#define glAttachShader p_glAttachShader
#define glBindAttribLocation p_glBindAttribLocation
#define glBindBuffer p_glBindBuffer
#define glBindFragDataLocation p_glBindFragDataLocation
#define glBindFramebuffer p_glBindFramebuffer
#define glBindRenderbuffer p_glBindRenderbuffer
#define glBindTexture p_glBindTexture
#define glBindVertexArray p_glBindVertexArray
#define glBlitFramebuffer p_glBlitFramebuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glClear p_glClear
#define glClearColor p_glClearColor
#define glClearDepth p_glClearDepth
#define glCompileShader p_glCompileShader
#define glCopyTexSubImage2D p_glCopyTexSubImage2D
#define glCreateProgram p_glCreateProgram
#define glCreateShader p_glCreateShader
#define glCullFace p_glCullFace
#define glDeleteBuffers p_glDeleteBuffers
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glDeleteProgram p_glDeleteProgram
#define glDeleteRenderbuffers p_glDeleteRenderbuffers
#define glDeleteShader p_glDeleteShader
#define glDeleteTextures p_glDeleteTextures
#define glDeleteVertexArrays p_glDeleteVertexArrays
#define glDepthFunc p_glDepthFunc
#define glDepthMask p_glDepthMask
#define glDisable p_glDisable
#define glDrawArrays p_glDrawArrays
#define glEnable p_glEnable
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFrontFace p_glFrontFace
#define glGenBuffers p_glGenBuffers
#define glGenFramebuffers p_glGenFramebuffers
#define glGenRenderbuffers p_glGenRenderbuffers
#define glGenTextures p_glGenTextures
#define glGenVertexArrays p_glGenVertexArrays
#define glGetProgramiv p_glGetProgramiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glGetShaderiv p_glGetShaderiv
#define glGetUniformLocation p_glGetUniformLocation
#define glLinkProgram p_glLinkProgram
#define glPixelStorei p_glPixelStorei
#define glPolygonOffset p_glPolygonOffset
#define glReadPixels p_glReadPixels
#define glRenderbufferStorage p_glRenderbufferStorage
#define glScissor p_glScissor
#define glShaderSource p_glShaderSource
#define glTexImage2D p_glTexImage2D
#define glTexImage3D p_glTexImage3D
#define glTexParameteri p_glTexParameteri
#define glTexSubImage2D p_glTexSubImage2D
#define glUniform1i p_glUniform1i
#define glUniform2f p_glUniform2f
#define glUniform4f p_glUniform4f
#define glUniformMatrix4fv p_glUniformMatrix4fv
#define glUseProgram p_glUseProgram
#define glVertexAttribPointer p_glVertexAttribPointer
#define glViewport p_glViewport

#else

int rnd_gl_load(void *(*get)(const char *name))
{
    (void)get;
    return 1;
}

#endif

/* A framebuffer name, with 0 the window's. */
#define FB(f) ((f) != 0 ? (GLuint)(f) : (GLuint)rnd_gl_screen_fbo)

enum { WORK_UNIT = 7 };

#if defined(IGN_GLES) && defined(__ANDROID__) && defined(IGN_GLES_FRAMEBUFFER_FETCH) && \
    IGN_GLES_FRAMEBUFFER_FETCH == 1
#define IGN_DYN_FETCH_BUILD 1
#else
#define IGN_DYN_FETCH_BUILD 0
#endif

static int s_dyn_framebuffer_fetch;

/* Optional GL state cache for Android's GLES backend. */
#if defined(IGN_GLES) && defined(__ANDROID__) && defined(IGN_GLES_STATE_CACHE) && \
    IGN_GLES_STATE_CACHE == 1
#define IGN_STATE_CACHE_BUILD 1
#else
#define IGN_STATE_CACHE_BUILD 0
#endif

#if IGN_STATE_CACHE_BUILD
enum { STATE_TEXTURE_UNITS = 32 };

struct cached_uniform {
    unsigned char valid;
    union {
        GLint i[16];
        GLfloat f[16];
    } value;
};

static struct {
    GLuint program, vao;
    int depth_test, depth_func, depth_write;
    int cull, cull_face, front_face;
    int offset, offset_factor, offset_units;
    int active_texture;
    GLuint tex2d[STATE_TEXTURE_UNITS], tex_array[STATE_TEXTURE_UNITS];
    struct cached_uniform uniforms[RND_PROGRAMS][RND_UNIFORMS];
} s_state;

static void state_cache_reset(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.program = (GLuint)~0u;
    s_state.vao = (GLuint)~0u;
    s_state.depth_test = -1;
    s_state.depth_func = -1;
    s_state.depth_write = -1;
    s_state.cull = -1;
    s_state.cull_face = -1;
    s_state.front_face = -1;
    s_state.offset = -1;
    s_state.active_texture = -1;
    memset(s_state.tex2d, 0xff, sizeof(s_state.tex2d));
    memset(s_state.tex_array, 0xff, sizeof(s_state.tex_array));
}

static void state_forget_program(int p)
{
    memset(s_state.uniforms[p], 0, sizeof(s_state.uniforms[p]));
}

static void state_use_program(GLuint program)
{
    if (s_state.program != program) {
        glUseProgram(program);
        s_state.program = program;
    }
}

static void state_bind_vao(GLuint vao)
{
    if (s_state.vao != vao) {
        glBindVertexArray(vao);
        s_state.vao = vao;
    }
}

static int state_active_texture(int unit)
{
    if ((unsigned)unit < STATE_TEXTURE_UNITS) {
        if (s_state.active_texture != unit) {
            glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
            s_state.active_texture = unit;
        }
        return unit;
    }
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    s_state.active_texture = -1;
    return -1;
}

static void state_bind_2d(int unit, GLuint tex)
{
    if (unit < 0) {
        glBindTexture(GL_TEXTURE_2D, tex);
    } else if (s_state.tex2d[unit] != tex) {
        glBindTexture(GL_TEXTURE_2D, tex);
        s_state.tex2d[unit] = tex;
    }
}

static void state_bind_array(int unit, GLuint tex)
{
    if (unit < 0) {
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    } else if (s_state.tex_array[unit] != tex) {
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        s_state.tex_array[unit] = tex;
    }
}

static void state_texture_freed(GLuint tex)
{
    int i;

    for (i = 0; i < STATE_TEXTURE_UNITS; i++) {
        if (s_state.tex2d[i] == tex) {
            s_state.tex2d[i] = 0;
        }
        if (s_state.tex_array[i] == tex) {
            s_state.tex_array[i] = 0;
        }
    }
}

static void state_uniform(int program, int id, GLint loc, const float *v)
{
    struct cached_uniform *u = &s_state.uniforms[program][id];

    switch (id) {
    case RND_U_MODE:
    case RND_U_USE_TABLE: {
        GLint value = (GLint)v[0];

        if (u->valid && u->value.i[0] == value) {
            return;
        }
        glUniform1i(loc, value);
        u->valid = 1;
        u->value.i[0] = value;
        break;
    }
    case RND_U_RECT:
        if (u->valid && memcmp(u->value.f, v, 4 * sizeof(float)) == 0) {
            return;
        }
        glUniform4f(loc, v[0], v[1], v[2], v[3]);
        u->valid = 1;
        memcpy(u->value.f, v, 4 * sizeof(float));
        break;
    case RND_U_MATRIX:
        if (u->valid && memcmp(u->value.f, v, 16 * sizeof(float)) == 0) {
            return;
        }
        glUniformMatrix4fv(loc, 1, GL_FALSE, v);
        u->valid = 1;
        memcpy(u->value.f, v, 16 * sizeof(float));
        break;
    default:
        if (u->valid && memcmp(u->value.f, v, 2 * sizeof(float)) == 0) {
            return;
        }
        glUniform2f(loc, v[0], v[1]);
        u->valid = 1;
        memcpy(u->value.f, v, 2 * sizeof(float));
        break;
    }
}
#else
static void state_cache_reset(void)
{
}

static void state_forget_program(int p)
{
    (void)p;
}

static void state_use_program(GLuint program)
{
    glUseProgram(program);
}

static void state_bind_vao(GLuint vao)
{
    glBindVertexArray(vao);
}

static int state_active_texture(int unit)
{
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    return -1;
}

static void state_bind_2d(int unit, GLuint tex)
{
    (void)unit;
    glBindTexture(GL_TEXTURE_2D, tex);
}

static void state_bind_array(int unit, GLuint tex)
{
    (void)unit;
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
}

static void state_texture_freed(GLuint tex)
{
    (void)tex;
}

static void state_uniform(int program, int id, GLint loc, const float *v)
{
    (void)program;
    (void)id;
    switch (id) {
    case RND_U_MODE:
    case RND_U_USE_TABLE:
        glUniform1i(loc, (GLint)v[0]);
        break;
    case RND_U_RECT:
        glUniform4f(loc, v[0], v[1], v[2], v[3]);
        break;
    case RND_U_MATRIX:
        glUniformMatrix4fv(loc, 1, GL_FALSE, v);
        break;
    default:
        glUniform2f(loc, v[0], v[1]);
        break;
    }
}
#endif

/* ------------------------------------------------------------ shaders */

/* CANVAS. Mode 0 copies the source index, 1 puts it through the table:
 * u_rows classes each source colour (0 leaves the target alone, 1 is
 * one colour, 2 reads the target). */
static const char *s_canvas_vs =
    "#version 150\n"
    "uniform vec2 u_lsize;\n"
    "uniform vec2 u_off;\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4((a_pos + u_off) / u_lsize * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *s_canvas_fs =
    "#version 150\n"
    "uniform sampler2D u_src;\n"
    "uniform sampler2D u_table;\n"
    "uniform sampler2D u_rows;\n"
    "uniform sampler2D u_dst;\n"
    "uniform int u_mode;\n"
    "in vec2 v_uv;\n"
    "out float o_index;\n"
    "void main() {\n"
    "    ivec2 size = textureSize(u_src, 0);\n"
    "    ivec2 t = clamp(ivec2(floor(v_uv)), ivec2(0), size - 1);\n"
    "    float s = texelFetch(u_src, t, 0).r;\n"
    "    if (u_mode == 1) {\n"
    "        int si = int(s * 255.0 + 0.5);\n"
    "        vec2 row = texelFetch(u_rows, ivec2(si, 0), 0).rg;\n"
    "        int cls = int(row.r * 255.0 + 0.5);\n"
    "        if (cls == 0) discard;\n"
    "        if (cls == 1) {\n"
    "            s = row.g;\n"
    "        } else {\n"
    "            float d = texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0).r;\n"
    "            s = texelFetch(u_table, ivec2(int(d * 255.0 + 0.5), si), 0).r;\n"
    "        }\n"
    "    }\n"
    "    o_index = s;\n"
    "}\n";

/* PRESENT and OVER: a quad from gl_VertexID; u_rect is x0, y0, x1, y1
 * in clip space. */
static const char *s_present_vs =
    "#version 150\n"
    "uniform vec4 u_rect;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
    "    v_uv = vec2(c.x, 1.0 - c.y);\n"
    "    gl_Position = vec4(mix(u_rect.xy, u_rect.zw, c), 0.0, 1.0);\n"
    "}\n";

static const char *s_present_fs =
    "#version 150\n"
    "uniform sampler2D u_index;\n"
    "uniform sampler2D u_palette;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    float i = texture(u_index, v_uv).r * 255.0;\n"
    "    o_color = vec4(texture(u_palette, vec2((i + 0.5) / 256.0, 0.5)).rgb, 1.0);\n"
    "}\n";

/* The screen's pixels where they differ from the snapshot taken after
 * the 3D was drawn into it: what the 2D drawing changed. */
static const char *s_over_fs =
    "#version 150\n"
    "uniform sampler2D u_index;\n"
    "uniform sampler2D u_palette;\n"
    "uniform sampler2D u_snap;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    float i = texture(u_index, v_uv).r * 255.0;\n"
    "    float s = texture(u_snap, v_uv).r * 255.0;\n"
    "    if (abs(i - s) < 0.5) discard;\n"
    "    o_color = vec4(texture(u_palette, vec2((i + 0.5) / 256.0, 0.5)).rgb, 1.0);\n"
    "}\n";

/* MESH: v_info is page, colour, mode (0 texel, 1 texel without colour
 * 0, 2 colour). Out: the index, and 1 for a covered pixel. */
static const char *s_mesh_vs =
    "#version 150\n"
    "uniform mat4 u_matrix;\n"
    "in vec3 a_pos;\n"
    "in vec2 a_uv;\n"
    "in vec3 a_info;\n"
    "out vec2 v_uv;\n"
    "flat out vec3 v_info;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_info = a_info;\n"
    "    gl_Position = u_matrix * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *s_mesh_fs =
    "#version 150\n"
    "uniform sampler2DArray u_pages;\n"
    "in vec2 v_uv;\n"
    "flat in vec3 v_info;\n"
    "out vec2 o_index;\n"
    "void main() {\n"
    "    float c = v_info.y;\n"
    "    if (v_info.z < 1.5) {\n"
    "        c = texture(u_pages, vec3(v_uv, v_info.x)).r * 255.0;\n"
    "        if (v_info.z > 0.5 && c < 0.5) discard;\n"
    "    }\n"
    "    o_index = vec2(c / 255.0, 1.0);\n"
    "}\n";

/* DYN: as MESH, clip space vertices, mode 3 a colour from u; through
 * the table with the pixel under it (a copy in u_dst) if u_use_table. */
static const char *s_dyn_vs =
    "#version 150\n"
    "in vec4 a_pos;\n"
    "in vec2 a_uv;\n"
    "in vec3 a_info;\n"
    "out vec2 v_uv;\n"
    "flat out vec3 v_info;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_info = a_info;\n"
    "    gl_Position = a_pos;\n"
    "}\n";

static const char *s_dyn_fs =
    "#version 150\n"
    "uniform sampler2DArray u_pages;\n"
    "uniform sampler2D u_table;\n"
    "uniform sampler2D u_dst;\n"
    "uniform int u_use_table;\n"
    "in vec2 v_uv;\n"
    "flat in vec3 v_info;\n"
#if IGN_DYN_FETCH_BUILD
    "#ifdef IGN_FRAMEBUFFER_FETCH\n"
    "inout vec2 o_index;\n"
    "#else\n"
#endif
    "out vec2 o_index;\n"
#if IGN_DYN_FETCH_BUILD
    "#endif\n"
#endif
    "void main() {\n"
    "    float c = v_info.z > 2.5 ? floor(v_uv.x) : v_info.y;\n"
    "    if (v_info.z < 1.5) {\n"
    "        c = texture(u_pages, vec3(v_uv, v_info.x)).r * 255.0;\n"
    "        if (v_info.z > 0.5 && c < 0.5) {\n"
    "            discard;\n"
    "        }\n"
    "        if (u_use_table == 1) {\n"
#if IGN_DYN_FETCH_BUILD
    "#ifdef IGN_FRAMEBUFFER_FETCH\n"
    "            float d = o_index.r * 255.0;\n"
    "#else\n"
#endif
    "            float d = texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0).r * 255.0;\n"
#if IGN_DYN_FETCH_BUILD
    "#endif\n"
#endif
    "            c = texelFetch(u_table, ivec2(int(d + 0.5), int(c + 0.5)), 0).r * 255.0;\n"
    "        }\n"
    "    } else if (u_use_table == 1) {\n"
#if IGN_DYN_FETCH_BUILD
    "#ifdef IGN_FRAMEBUFFER_FETCH\n"
    "        float d = o_index.r * 255.0;\n"
    "#else\n"
#endif
    "        float d = texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0).r * 255.0;\n"
#if IGN_DYN_FETCH_BUILD
    "#endif\n"
#endif
    "        c = texelFetch(u_table, ivec2(int(d + 0.5), int(c + 0.5)), 0).r * 255.0;\n"
    "    }\n"
    "    o_index = vec2(c / 255.0, 1.0);\n"
    "}\n";

/* RASTER: pixels in, the index out. Modes: 0 the texel, 1 the colour,
 * 2 the texel and 3 the colour through the table with the pixel under
 * them. Vertices go half a pixel left and up, as the original fills a
 * pixel whose bottom right corner is inside. */
static const char *s_raster_vs =
    "#version 150\n"
    "uniform vec2 u_size;\n"
    "uniform vec2 u_off;\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in vec3 a_info;\n"
    "out vec2 v_uv;\n"
    "flat out vec3 v_info;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_info = a_info;\n"
    "    gl_Position = vec4((a_pos - 0.5 + u_off) / u_size * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *s_raster_fs =
    "#version 150\n"
    "uniform sampler2DArray u_pages;\n"
    "uniform sampler2D u_table;\n"
    "uniform sampler2D u_dst;\n"
    "in vec2 v_uv;\n"
    "flat in vec3 v_info;\n"
    "out float o_index;\n"
    "void main() {\n"
    "    float mode = v_info.z;\n"
    "    float a = v_info.y;\n"
    "    if (mode < 0.5 || (mode > 1.5 && mode < 2.5)) {\n"
    "        ivec2 t = ivec2(floor(v_uv)) & 255;\n"
    "        a = texelFetch(u_pages, ivec3(t, int(v_info.x)), 0).r * 255.0;\n"
    "    }\n"
    "    if (mode > 1.5) {\n"
    "        float d = texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0).r * 255.0;\n"
    "        a = texelFetch(u_table, ivec2(int(d + 0.5), int(a + 0.5)), 0).r * 255.0;\n"
    "    }\n"
    "    o_index = a / 255.0;\n"
    "}\n";

/* Each program's vertex attributes (components each, 0 ends), samplers
 * by unit, fragment output and first vertex buffer size. */
static const struct {
    int attrs[4];
    const char *attr_names[3];
    const char *samplers[4];
    const char *out;
    int buffer_verts;
} s_def[RND_PROGRAMS] = {
    { { 2, 2, 0 }, { "a_pos", "a_uv" }, { "u_src", "u_table", "u_rows", "u_dst" }, "o_index", 4 },
    { { 0 }, { NULL }, { "u_index", "u_palette" }, NULL, 0 },
    { { 0 }, { NULL }, { "u_index", "u_palette", "u_snap" }, "o_color", 0 },
    { { 3, 2, 3, 0 }, { "a_pos", "a_uv", "a_info" }, { "u_pages" }, "o_index", 0 },
    { { 4, 2, 3, 0 }, { "a_pos", "a_uv", "a_info" }, { "u_pages", "u_table", "u_dst" },
      "o_index", 6 },
    { { 2, 2, 3, 0 }, { "a_pos", "a_uv", "a_info" }, { "u_pages", "u_table", "u_dst" },
      "o_index", 0 },
};

static const char *s_uniform_names[RND_UNIFORMS] = {
    "u_lsize", "u_off", "u_mode", "u_rect", "u_matrix", "u_use_table", "u_size"
};

static struct {
    GLuint program, vao, vbo;
    int stride;                 /* floats a vertex */
    GLsizeiptr cap;             /* bytes in vbo */
    GLint uniforms[RND_UNIFORMS];
} s_prog[RND_PROGRAMS];

static int s_use = -1;
static GLuint s_mesh_vbo;

/* What each GL texture name is. */
struct tex {
    GLenum target;
    int format, w, h;
};

static struct tex *s_tex;
static unsigned s_tex_cap;

/* Each framebuffer's depth buffer and colour texture. */
struct fb {
    GLuint depth, color;
};

static struct fb *s_fb;
static unsigned s_fb_cap;
static GLuint s_bound;

static void *grow(void *p, unsigned *cap, unsigned need, size_t size)
{
    unsigned n = *cap;

    if (need < n) {
        return p;
    }
    while (n <= need) {
        n = n == 0 ? 64 : n * 2;
    }
    p = realloc(p, n * size);
    memset((char *)p + *cap * size, 0, (n - *cap) * size);
    *cap = n;
    return p;
}

static struct tex *tex_rec(GLuint t)
{
    s_tex = grow(s_tex, &s_tex_cap, t, sizeof(*s_tex));
    return &s_tex[t];
}

static struct fb *fb_rec(GLuint f)
{
    s_fb = grow(s_fb, &s_fb_cap, f, sizeof(*s_fb));
    return &s_fb[f];
}

static GLuint compile(GLenum type, const char *src, int fetch)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;

#if defined(IGN_GLES)
    static const char es[] =
        "#version 300 es\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n"
        "precision highp sampler2DArray;\n";
    static const char es_fetch[] =
        "#version 300 es\n"
        "#extension GL_EXT_shader_framebuffer_fetch : require\n"
        "#define IGN_FRAMEBUFFER_FETCH 1\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n"
        "precision highp sampler2DArray;\n";
    const char *parts[2];

    parts[0] = fetch ? es_fetch : es;
    parts[1] = strncmp(src, "#version 150\n", 13) == 0 ? src + 13 : src;
    glShaderSource(s, 2, parts, NULL);
#else
    (void)fetch;
    glShaderSource(s, 1, &src, NULL);
#endif
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];

        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "rnd: shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int make_program(int p, const char *vs_src, const char *fs_src, int fetch)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src, 0);
    GLuint fs;
    GLint ok = 0;
    int i, offset = 0, stride = 0;

    if (vs == 0) {
        return 0;
    }
    fs = compile(GL_FRAGMENT_SHADER, fs_src, fetch);
    if (fs == 0) {
        glDeleteShader(vs);
        return 0;
    }
    state_forget_program(p);
    s_prog[p].program = 0;
    s_prog[p].vao = 0;
    s_prog[p].vbo = 0;
    s_prog[p].cap = 0;
    s_prog[p].program = glCreateProgram();
    glAttachShader(s_prog[p].program, vs);
    glAttachShader(s_prog[p].program, fs);
    for (i = 0; s_def[p].attrs[i] != 0; i++) {
        glBindAttribLocation(s_prog[p].program, (GLuint)i, s_def[p].attr_names[i]);
        stride += s_def[p].attrs[i];
    }
    if (p == RND_RASTER) {
        stride = 8;             /* and a float unused */
    }
#if !defined(IGN_GLES)
    if (s_def[p].out != NULL) {
        glBindFragDataLocation(s_prog[p].program, 0, s_def[p].out);
    }
#endif
    glLinkProgram(s_prog[p].program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(s_prog[p].program, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "rnd: program %d: link failed\n", p);
        glDeleteProgram(s_prog[p].program);
        s_prog[p].program = 0;
        return 0;
    }
    state_use_program(s_prog[p].program);
    for (i = 0; i < 4 && s_def[p].samplers[i] != NULL; i++) {
        glUniform1i(glGetUniformLocation(s_prog[p].program, s_def[p].samplers[i]), i);
    }
    for (i = 0; i < RND_UNIFORMS; i++) {
        s_prog[p].uniforms[i] = glGetUniformLocation(s_prog[p].program, s_uniform_names[i]);
    }
    s_prog[p].stride = stride;

    glGenVertexArrays(1, &s_prog[p].vao);
    state_bind_vao(s_prog[p].vao);
    if (stride == 0) {
        return 1;
    }
    if (p == RND_MESH) {
        glGenBuffers(1, &s_mesh_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_mesh_vbo);
    } else {
        glGenBuffers(1, &s_prog[p].vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_prog[p].vbo);
        s_prog[p].cap = (GLsizeiptr)s_def[p].buffer_verts * stride * (GLsizeiptr)sizeof(float);
        if (s_prog[p].cap > 0) {
            glBufferData(GL_ARRAY_BUFFER, s_prog[p].cap, NULL, GL_STREAM_DRAW);
        }
    }
    for (i = 0; s_def[p].attrs[i] != 0; i++) {
        glEnableVertexAttribArray((GLuint)i);
        glVertexAttribPointer((GLuint)i, s_def[p].attrs[i], GL_FLOAT, GL_FALSE,
                              stride * (GLsizei)sizeof(float), (void *)(offset * sizeof(float)));
        offset += s_def[p].attrs[i];
    }
    return 1;
}

static int dyn_fetch_extension(void)
{
#if IGN_DYN_FETCH_BUILD
    GLint count = 0, i;

    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (i = 0; i < count; i++) {
        const char *ext = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);

        if (ext != NULL && strcmp(ext, "GL_EXT_shader_framebuffer_fetch") == 0) {
            return 1;
        }
    }
    return 0;
#else
    return 0;
#endif
}

int rnd_init(void)
{
    const char *src[RND_PROGRAMS][2] = {
        { s_canvas_vs, s_canvas_fs }, { s_present_vs, s_present_fs },
        { s_present_vs, s_over_fs }, { s_mesh_vs, s_mesh_fs },
        { s_dyn_vs, s_dyn_fs }, { s_raster_vs, s_raster_fs },
    };
    int p, fetch = 0;

    state_cache_reset();
    s_dyn_framebuffer_fetch = 0;
    for (p = 0; p < RND_PROGRAMS; p++) {
        if (p == RND_DYN && dyn_fetch_extension()) {
            if (make_program(p, src[p][0], src[p][1], 1)) {
                fetch = 1;
            } else if (!make_program(p, src[p][0], src[p][1], 0)) {
                state_cache_reset();
                return 0;
            }
        } else if (!make_program(p, src[p][0], src[p][1], 0)) {
            state_cache_reset();
            return 0;
        }
    }
    s_dyn_framebuffer_fetch = fetch;
    s_use = -1;
    state_cache_reset();
    return 1;
}

void rnd_shutdown(void)
{
    int p;

    s_dyn_framebuffer_fetch = 0;
    for (p = 0; p < RND_PROGRAMS; p++) {
        glDeleteProgram(s_prog[p].program);
        glDeleteVertexArrays(1, &s_prog[p].vao);
        if (s_prog[p].vbo != 0) {
            glDeleteBuffers(1, &s_prog[p].vbo);
        }
    }
    glDeleteBuffers(1, &s_mesh_vbo);
    state_cache_reset();
}

#if defined(IGN_GLES)
int rnd_dyn_framebuffer_fetch(void)
{
    return s_dyn_framebuffer_fetch;
}
#endif

/* ------------------------------------------------------------ textures */

static GLenum gl_internal(int format)
{
    return format == RND_RG8 ? GL_RG8 : format == RND_RGB8 ? GL_RGB8 : GL_R8;
}

static GLenum gl_format(int format)
{
    return format == RND_RG8 ? GL_RG : format == RND_RGB8 ? GL_RGB : GL_RED;
}

unsigned rnd_texture(int format, int w, int h, const void *pixels, int pitch)
{
    GLuint t;
    struct tex *r;
    int unit;

    glGenTextures(1, &t);
    unit = state_active_texture(WORK_UNIT);
    state_bind_2d(unit, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pixels != NULL && pitch != 0 && pitch != w ? pitch : 0);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)gl_internal(format), w, h, 0, gl_format(format),
                 GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    r = tex_rec(t);
    r->target = GL_TEXTURE_2D;
    r->format = format;
    r->w = w;
    r->h = h;
    return t;
}

void rnd_texture_set(unsigned tex, int w, int h, const void *pixels, int pitch)
{
    struct tex *r = tex_rec(tex);
    int unit;

    unit = state_active_texture(WORK_UNIT);
    state_bind_2d(unit, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch != 0 && pitch != w ? pitch : 0);
    if (w != r->w || h != r->h) {
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)gl_internal(r->format), w, h, 0,
                     gl_format(r->format), GL_UNSIGNED_BYTE, pixels);
        r->w = w;
        r->h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, gl_format(r->format), GL_UNSIGNED_BYTE,
                        pixels);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void rnd_texture_free(unsigned tex)
{
    GLuint t = tex;

    if (t != 0) {
        glDeleteTextures(1, &t);
        state_texture_freed(t);
        memset(tex_rec(t), 0, sizeof(struct tex));
    }
}

unsigned rnd_pages(const u8 *pixels, int count)
{
    GLuint t;
    struct tex *r;
    int unit;

    glGenTextures(1, &t);
    unit = state_active_texture(WORK_UNIT);
    state_bind_array(unit, t);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, 256, 256, count, 0, GL_RED, GL_UNSIGNED_BYTE,
                 pixels);
    r = tex_rec(t);
    r->target = GL_TEXTURE_2D_ARRAY;
    r->format = RND_R8;
    r->w = 256;
    r->h = 256;
    return t;
}

#if IGN_SHARED_PAGES_BUILD

static struct {
    GLuint tex;
    GLint layers;
} s_page_pool;

static void clear_gl_errors(void)
{
    while (glGetError() != GL_NO_ERROR) {
    }
}

unsigned rnd_page_pool_create(int *layers)
{
    GLint max_layers = 0, count;
    GLuint t = 0;
    struct tex *r;

    *layers = 0;
    clear_gl_errors();
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
    count = max_layers > RND_PAGE_POOL_MAX_LAYERS ? RND_PAGE_POOL_MAX_LAYERS : max_layers;
    if (count < 1) {
        return 0;
    }
    glGenTextures(1, &t);
    if (t == 0) {
        return 0;
    }
    state_bind_array(state_active_texture(WORK_UNIT), t);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, 256, 256, count, 0, GL_RED,
                 GL_UNSIGNED_BYTE, NULL);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &t);
        state_texture_freed(t);
        return 0;
    }
    r = tex_rec(t);
    r->target = GL_TEXTURE_2D_ARRAY;
    r->format = RND_R8;
    r->w = 256;
    r->h = 256;
    s_page_pool.tex = t;
    s_page_pool.layers = count;
    *layers = (int)count;
    return t;
}

int rnd_page_pool_upload(unsigned tex, int base_layer, const u8 *pixels, int pages)
{
    if (tex == 0 || tex != s_page_pool.tex) {
        return 0;
    }
    if (pixels == NULL || pages <= 0 || base_layer < 0 ||
        base_layer > (int)s_page_pool.layers - pages) {
        return 0;
    }
    clear_gl_errors();
    state_bind_array(state_active_texture(WORK_UNIT), tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, base_layer, 256, 256, pages, GL_RED,
                    GL_UNSIGNED_BYTE, pixels);
    return glGetError() == GL_NO_ERROR;
}

void rnd_page_pool_release(unsigned tex)
{
    if (tex == 0 || tex != s_page_pool.tex) {
        return;
    }
    rnd_texture_free(tex);
    s_page_pool.tex = 0;
    s_page_pool.layers = 0;
}

#endif

void rnd_texture_size(unsigned tex, int *w, int *h)
{
    struct tex *r = tex_rec(tex);

    *w = r->w;
    *h = r->h;
}

/* ------------------------------------------------------------ targets */

unsigned rnd_target(unsigned color, int depth)
{
    GLuint f;
    struct fb *r;
    struct tex *c = tex_rec(color);

    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    r = fb_rec(f);
    r->color = color;
    r->depth = 0;
    if (depth) {
        glGenRenderbuffers(1, &r->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, r->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, c->w, c->h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, r->depth);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, FB(s_bound));
    return f;
}

void rnd_target_free(unsigned target)
{
    GLuint f = target;
    struct fb *r;

    if (f == 0) {
        return;
    }
    r = fb_rec(f);
    if (r->depth != 0) {
        glDeleteRenderbuffers(1, &r->depth);
    }
    glDeleteFramebuffers(1, &f);
    memset(r, 0, sizeof(*r));
    if (s_bound == f) {
        s_bound = 0;
    }
}

void rnd_bind(unsigned target)
{
    struct tex *c = tex_rec(fb_rec(target)->color);

    s_bound = target;
    glBindFramebuffer(GL_FRAMEBUFFER, FB(target));
    glViewport(0, 0, c->w, c->h);
}

void rnd_bind_screen(int w, int h)
{
    s_bound = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, FB(0));
    glViewport(0, 0, w, h);
}

void rnd_clear(const float *rgba, int depth)
{
    GLbitfield mask = 0;

    if (rgba != NULL) {
        glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (depth) {
#if defined(IGN_GLES)
        glClearDepthf(1.0f);
#else
        glClearDepth(1.0);
#endif
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    glClear(mask);
}

void rnd_read(int x, int y, int w, int h, int format, void *out, int pitch)
{
    int bpp = format == RND_RG8 ? 2 : format == RND_RGB8 ? 3 : 1;

#if defined(IGN_GLES)
    u8 *rgba = malloc((size_t)w * (size_t)h * 4);
    int r, c, k;

    if (rgba == NULL) {
        return;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (r = 0; r < h; r++) {
        for (c = 0; c < w; c++) {
            for (k = 0; k < bpp; k++) {
                ((u8 *)out)[r * pitch + c * bpp + k] = rgba[(r * w + c) * 4 + k];
            }
        }
    }
    free(rgba);
    return;
#endif
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, pitch != w * bpp ? pitch / bpp : 0);
    glReadPixels(x, y, w, h, gl_format(format), GL_UNSIGNED_BYTE, out);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

void rnd_copy(unsigned tex, int x, int y, int w, int h)
{
    int unit;

    prof_count(PROF_COPY_CALLS, 1);
    prof_count(PROF_COPY_PIXELS, (uint64_t)w * (uint64_t)h);
    unit = state_active_texture(WORK_UNIT);
    state_bind_2d(unit, tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x, y, x, y, w, h);
}

void rnd_blit(unsigned src, unsigned dst, int sw, int sh, int dw, int dh)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, FB(src));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, FB(dst));
    glBlitFramebuffer(0, 0, sw, sh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, FB(s_bound));
}

/* ------------------------------------------------------------ state */

void rnd_scissor(int on, int x, int y, int w, int h)
{
    if (on) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(x, y, w, h);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

void rnd_depth(int func, int write)
{
#if IGN_STATE_CACHE_BUILD
    int test = func != RND_DEPTH_OFF;

    if (s_state.depth_test != test) {
        if (test) {
            glEnable(GL_DEPTH_TEST);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        s_state.depth_test = test;
    }
    if (test && s_state.depth_func != func) {
        glDepthFunc(func == RND_DEPTH_LESS ? GL_LESS : GL_LEQUAL);
        s_state.depth_func = func;
    }
    if (s_state.depth_write != write) {
        glDepthMask(write ? GL_TRUE : GL_FALSE);
        s_state.depth_write = write;
    }
#else
    if (func == RND_DEPTH_OFF) {
        glDisable(GL_DEPTH_TEST);
    } else {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(func == RND_DEPTH_LESS ? GL_LESS : GL_LEQUAL);
    }
    glDepthMask(write ? GL_TRUE : GL_FALSE);
#endif
}

void rnd_cull(int mode)
{
#if IGN_STATE_CACHE_BUILD
    int front;

    if (mode == RND_CULL_OFF) {
        if (s_state.cull != 0) {
            glDisable(GL_CULL_FACE);
            s_state.cull = 0;
        }
        return;
    }
    front = mode == RND_CULL_CCW ? GL_CW : GL_CCW;
    if (s_state.cull != 1) {
        glEnable(GL_CULL_FACE);
        s_state.cull = 1;
    }
    if (s_state.cull_face != GL_BACK) {
        glCullFace(GL_BACK);
        s_state.cull_face = GL_BACK;
    }
    if (s_state.front_face != front) {
        glFrontFace(front);
        s_state.front_face = front;
    }
#else
    if (mode == RND_CULL_OFF) {
        glDisable(GL_CULL_FACE);
        return;
    }
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(mode == RND_CULL_CCW ? GL_CW : GL_CCW);
#endif
}

void rnd_offset(int on)
{
#if IGN_STATE_CACHE_BUILD
    if (on) {
        int changed = s_state.offset_factor != -1;

        if (s_state.offset != 1) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            s_state.offset = 1;
        }
        if (s_state.offset_units != -4) {
            changed = 1;
        }
        if (changed) {
            glPolygonOffset(-1.0f, -4.0f);
            s_state.offset_factor = -1;
            s_state.offset_units = -4;
        }
    } else if (s_state.offset != 0) {
        glDisable(GL_POLYGON_OFFSET_FILL);
        s_state.offset = 0;
    }
#else
    if (on) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -4.0f);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
#endif
}

/* ------------------------------------------------------------ drawing */

void rnd_use(int program)
{
    s_use = program;
    state_use_program(s_prog[program].program);
    state_bind_vao(s_prog[program].vao);
}

void rnd_uniform(int id, const float *v)
{
    GLint loc = s_prog[s_use].uniforms[id];

    state_uniform(s_use, id, loc, v);
}

void rnd_texture_unit(int unit, unsigned tex)
{
    int cached = state_active_texture(unit);

    if (tex == 0) {
        state_bind_2d(cached, 0);
        state_bind_array(cached, 0);
        return;
    }
    if (tex_rec(tex)->target == GL_TEXTURE_2D_ARRAY) {
        state_bind_array(cached, tex);
    } else {
        state_bind_2d(cached, tex);
    }
}

void rnd_stream(const float *verts, int count)
{
    GLsizeiptr bytes = (GLsizeiptr)count * s_prog[s_use].stride * (GLsizeiptr)sizeof(float);

    prof_count(PROF_UPLOAD_BYTES, (uint64_t)bytes);
    glBindBuffer(GL_ARRAY_BUFFER, s_prog[s_use].vbo);
    if (bytes > s_prog[s_use].cap) {
        glBufferData(GL_ARRAY_BUFFER, bytes, NULL, GL_STREAM_DRAW);
        s_prog[s_use].cap = bytes;
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, verts);
}

/* A whole pass at once. The orphan lets draws already queued keep
 * reading the old storage, so a later pass cannot overwrite their
 * vertices. The retained size is the largest pass seen, bounded by the
 * caller's face limit. */
void rnd_stream_pass(const float *verts, int count)
{
    GLsizeiptr bytes = (GLsizeiptr)count * s_prog[s_use].stride * (GLsizeiptr)sizeof(float);
    GLsizeiptr cap = s_prog[s_use].cap;

    prof_count(PROF_UPLOAD_BYTES, (uint64_t)bytes);
    glBindBuffer(GL_ARRAY_BUFFER, s_prog[s_use].vbo);
    if (bytes > cap) {
        cap = bytes;
        s_prog[s_use].cap = cap;
    }
    glBufferData(GL_ARRAY_BUFFER, cap, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, verts);
}

void rnd_draw(int prim, int first, int count)
{
    prof_count(PROF_DRAW_CALLS, 1);
    glDrawArrays(prim == RND_STRIP ? GL_TRIANGLE_STRIP : GL_TRIANGLES, first, count);
}

void rnd_mesh_store(int count)
{
    glBindBuffer(GL_ARRAY_BUFFER, s_mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * 8 * (GLsizeiptr)sizeof(float), NULL,
                 GL_STATIC_DRAW);
}

void rnd_mesh_write(int first, int count, const float *verts)
{
    prof_count(PROF_UPLOAD_BYTES, (uint64_t)count * 8 * sizeof(float));
    glBindBuffer(GL_ARRAY_BUFFER, s_mesh_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)first * 8 * (GLintptr)sizeof(float),
                    (GLsizeiptr)count * 8 * (GLsizeiptr)sizeof(float), verts);
}

void rnd_draw_mesh(int first, int count)
{
    prof_count(PROF_DRAW_CALLS, 1);
    glDrawArrays(GL_TRIANGLES, first, count);
}

void rnd_present(void)
{
}

/* Copies are dear on the tile-based Android path, which defaults to one
 * whole-frame copy per dependency layer. IGN_GLES_BATCH_COPIES=0 turns that
 * off and =1 enables it on any IGN_GLES build; other builds keep a copy per
 * face. */
int rnd_batch_copies(void)
{
#if defined(IGN_GLES) && defined(IGN_GLES_BATCH_COPIES)
    return IGN_GLES_BATCH_COPIES == 1;
#elif defined(IGN_GLES) && defined(__ANDROID__)
    return 1;
#else
    return 0;
#endif
}
