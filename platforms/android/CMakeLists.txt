#------------------------------------------------------------------------------
# Ignition - Android native library
#------------------------------------------------------------------------------
#
# Built per ABI by build.sh with the NDK's toolchain file: libignition.so,
# the engine and src/platform/android.c (NativeActivity, EGL with OpenGL
# ES 3.0, AAudio) with the NDK's android_native_app_glue. Nothing else.

cmake_minimum_required(VERSION 3.16)
project(ignition_android C)

set(IGN_VERSION "0.0.0" CACHE STRING "This port's version, e.g. 0.1.0")

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SRC ${ROOT}/src)
file(GLOB IGN_SOURCES CONFIGURE_DEPENDS ${SRC}/*.c ${SRC}/lisa/*.c ${SRC}/race/*.c ${SRC}/menu/*.c
     ${SRC}/frontend/*.c ${SRC}/soft3d/*.c)
list(FILTER IGN_SOURCES EXCLUDE REGEX "/rnd_gxm\\.c$")
list(APPEND IGN_SOURCES ${SRC}/platform/android.c ${SRC}/platform/common.c ${SRC}/platform/udp.c
     ${SRC}/platform/touch.c)

set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(ignition SHARED ${IGN_SOURCES} ${GLUE}/android_native_app_glue.c)
set_property(TARGET ignition PROPERTY C_STANDARD 99)
target_include_directories(ignition PRIVATE ${SRC} ${GLUE})
target_compile_definitions(ignition PRIVATE IGN_GLES _DEFAULT_SOURCE IGN_VERSION="${IGN_VERSION}")

# Android's renderer paths are part of the normal build. GPU capability and
# allocation/shader failures still select their safe runtime fallbacks.
foreach(feature IN ITEMS IGN_GLES_FRAMEBUFFER_FETCH IGN_GLES_DYN_ARENA IGN_GLES_VISIBILITY_CULL IGN_GLES_STATE_CACHE IGN_GLES_SHARED_PAGES)
    target_compile_definitions(ignition PRIVATE ${feature}=1)
endforeach()
# android_main calls main.c's main under this name.
set_source_files_properties(${SRC}/main.c PROPERTIES COMPILE_DEFINITIONS main=ign_main)

# -fsigned-char: the game is x86 code, where char is signed.
target_compile_options(ignition PRIVATE -fsigned-char -O2
    -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation)

# The glue's entry point is only reached from the system, by name.
target_link_options(ignition PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(ignition PRIVATE android EGL GLESv3 aaudio log m)
