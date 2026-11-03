/*
 * vorbis.c - stb_vorbis (public domain), for the GOG and Steam releases'
 * music, which is Ogg Vorbis. The decoder's own warnings are not ours.
 */

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

#define STB_VORBIS_NO_PUSHDATA_API
#include "third_party/stb_vorbis.c"
