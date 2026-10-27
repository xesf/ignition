/*
 * A small GLES 3 surface for host unit tests of rnd_gl.c. The test
 * executable supplies the functions and records the calls it needs.
 */

#ifndef IGN_TEST_FAKE_GLES3_GL3_H
#define IGN_TEST_FAKE_GLES3_GL3_H

#include <stddef.h>

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;
typedef float GLfloat;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef char GLchar;
typedef unsigned char GLubyte;

enum {
    GL_FALSE = 0,
    GL_TRUE = 1,
    GL_NO_ERROR = 0,
    GL_INVALID_OPERATION = 0x0502,
    GL_OUT_OF_MEMORY = 0x0505,
    GL_TRIANGLES = 0x0004,
    GL_TRIANGLE_STRIP = 0x0005,
    GL_LESS = 0x0201,
    GL_LEQUAL = 0x0203,
    GL_CULL_FACE = 0x0b44,
    GL_DEPTH_TEST = 0x0b71,
    GL_SCISSOR_TEST = 0x0c11,
    GL_POLYGON_OFFSET_FILL = 0x8037,
    GL_UNPACK_ROW_LENGTH = 0x0cf2,
    GL_PACK_ROW_LENGTH = 0x0d02,
    GL_UNPACK_ALIGNMENT = 0x0cf5,
    GL_PACK_ALIGNMENT = 0x0d05,
    GL_TEXTURE_2D = 0x0de1,
    GL_TEXTURE_2D_ARRAY = 0x8c1a,
    GL_TEXTURE0 = 0x84c0,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_NEAREST = 0x2600,
    GL_REPEAT = 0x2901,
    GL_CLAMP_TO_EDGE = 0x812f,
    GL_RED = 0x1903,
    GL_RG = 0x8227,
    GL_RGB = 0x1907,
    GL_RGBA = 0x1908,
    GL_R8 = 0x8229,
    GL_RG8 = 0x822b,
    GL_RGB8 = 0x8051,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_FLOAT = 0x1406,
    GL_STREAM_DRAW = 0x88e0,
    GL_STATIC_DRAW = 0x88e4,
    GL_ARRAY_BUFFER = 0x8892,
    GL_FRAGMENT_SHADER = 0x8b30,
    GL_VERTEX_SHADER = 0x8b31,
    GL_COMPILE_STATUS = 0x8b81,
    GL_LINK_STATUS = 0x8b82,
    GL_FRAMEBUFFER = 0x8d40,
    GL_READ_FRAMEBUFFER = 0x8ca8,
    GL_DRAW_FRAMEBUFFER = 0x8ca9,
    GL_RENDERBUFFER = 0x8d41,
    GL_COLOR_ATTACHMENT0 = 0x8ce0,
    GL_DEPTH_ATTACHMENT = 0x8d00,
    GL_DEPTH_COMPONENT24 = 0x81a6,
    GL_COLOR_BUFFER_BIT = 0x00004000,
    GL_DEPTH_BUFFER_BIT = 0x00000100,
    GL_BACK = 0x0405,
    GL_CW = 0x0900,
    GL_CCW = 0x0901,
    GL_EXTENSIONS = 0x1f03,
    GL_NUM_EXTENSIONS = 0x821d,
    GL_MAX_ARRAY_TEXTURE_LAYERS = 0x88ff
};

void glActiveTexture(GLenum texture);
void glAttachShader(GLuint program, GLuint shader);
void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name);
void glBindBuffer(GLenum target, GLuint buffer);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
void glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void glBindTexture(GLenum target, GLuint texture);
void glBindVertexArray(GLuint array);
void glBlitFramebuffer(GLint src_x0, GLint src_y0, GLint src_x1, GLint src_y1,
                       GLint dst_x0, GLint dst_y0, GLint dst_x1, GLint dst_y1,
                       GLbitfield mask, GLenum filter);
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
void glClear(GLbitfield mask);
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glClearDepthf(GLfloat depth);
void glCompileShader(GLuint shader);
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLint x, GLint y, GLsizei width, GLsizei height);
GLuint glCreateProgram(void);
GLuint glCreateShader(GLenum type);
void glCullFace(GLenum mode);
void glDeleteBuffers(GLsizei n, const GLuint *buffers);
void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
void glDeleteProgram(GLuint program);
void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers);
void glDeleteShader(GLuint shader);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glDeleteVertexArrays(GLsizei n, const GLuint *arrays);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glDisable(GLenum cap);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glEnable(GLenum cap);
void glEnableVertexAttribArray(GLuint index);
void glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                               GLenum renderbuffertarget, GLuint renderbuffer);
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                            GLuint texture, GLint level);
void glFrontFace(GLenum mode);
void glGenBuffers(GLsizei n, GLuint *buffers);
void glGenFramebuffers(GLsizei n, GLuint *framebuffers);
void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers);
void glGenTextures(GLsizei n, GLuint *textures);
void glGenVertexArrays(GLsizei n, GLuint *arrays);
void glGetIntegerv(GLenum pname, GLint *data);
GLenum glGetError(void);
void glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void glGetShaderInfoLog(GLuint shader, GLsizei buf_size, GLsizei *length, GLchar *info_log);
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
const GLubyte *glGetStringi(GLenum name, GLuint index);
GLint glGetUniformLocation(GLuint program, const GLchar *name);
void glLinkProgram(GLuint program);
void glPixelStorei(GLenum pname, GLint param);
void glPolygonOffset(GLfloat factor, GLfloat units);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                  GLenum type, void *pixels);
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width,
                           GLsizei height);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                    const GLint *length);
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                  GLsizei height, GLint border, GLenum format, GLenum type,
                  const void *pixels);
void glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                  GLsizei height, GLsizei depth, GLint border, GLenum format,
                  GLenum type, const void *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const void *pixels);
void glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                     GLenum format, GLenum type, const void *pixels);
void glUniform1i(GLint location, GLint v0);
void glUniform2f(GLint location, GLfloat v0, GLfloat v1);
void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *value);
void glUseProgram(GLuint program);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                           GLsizei stride, const void *pointer);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

#endif
