#------------------------------------------------------------------------------
# Ignition - PlayStation Vita build
#------------------------------------------------------------------------------
#
#   cmake -S platforms/vita -B build-vita
#   cmake --build build-vita
#       -> build-vita/ignition.vpk (title IGNT00001)
#
# Or "make vita" from the top. Needs VitaSDK ($VITASDK, or ~/vitasdk);
# its system libraries only. Compiles ../../src directly; the Vita parts
# are src/platform/vita.c and src/rnd_gxm.c (GXM). See docs/vita.md.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{VITASDK} AND EXISTS "$ENV{HOME}/vitasdk")
    set(ENV{VITASDK} "$ENV{HOME}/vitasdk")
endif()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "VITASDK is not set: install VitaSDK and export VITASDK")
    endif()
endif()

project(ignition_vita C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SRC ${ROOT}/src)

# Every engine file, the Vita host instead of the others, and the GXM
# renderer instead of the OpenGL one.
file(GLOB IGN_SOURCES CONFIGURE_DEPENDS ${SRC}/*.c ${SRC}/lisa/*.c ${SRC}/race/*.c ${SRC}/menu/*.c
     ${SRC}/frontend/*.c ${SRC}/soft3d/*.c)
list(FILTER IGN_SOURCES EXCLUDE REGEX "/rnd_gl\\.c$")
list(APPEND IGN_SOURCES ${SRC}/platform/vita.c)

add_executable(ignition ${IGN_SOURCES})
target_include_directories(ignition PRIVATE ${SRC})
# -fsigned-char: the game is x86 code, where char is signed.
target_compile_options(ignition PRIVATE
    -O2 -fsigned-char -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation)
# IGN_PROF: the frame's parts timed in the log (src/prof.h).
target_compile_definitions(ignition PRIVATE IGN_PROF)
# Room after the code for the module info vita-elf-create appends (the
# SDK's linker script reads __sce_headroom).
target_link_options(ignition PRIVATE -Wl,--defsym,__sce_headroom=0x4000)
target_link_libraries(ignition
    SceGxm_stub
    SceDisplay_stub
    SceSysmodule_stub
    SceNet_stub
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    ScePower_stub
    SceAppMgr_stub
    SceIofilemgr_stub
    SceLibKernel_stub
    SceShaccCg_stub_weak        # resolved only once libshacccg.suprx is loaded
    m
    c
)

# LiveArea art, by tools/vita_sce_sys.py: from the game's own pictures
# when an extracted disc is found (IGN_DISC, default the first
# data/*/disc or data/*/*/disc), else drawn.
if(NOT DEFINED IGN_DISC)
    # An extracted disc, under data/ or one folder down; the shortest
    # name, as the game picks it (the plain image before its rereleases).
    file(GLOB DISCS ${ROOT}/data/*/disc ${ROOT}/data/*/*/disc)
    set(IGN_DISC "")
    set(best 100000)
    foreach(d ${DISCS})
        string(LENGTH "${d}" len)
        if(len LESS best)
            set(best ${len})
            set(IGN_DISC "${d}")
        endif()
    endforeach()
endif()
set(SYS ${CMAKE_CURRENT_BINARY_DIR}/sce_sys)
add_custom_command(
    OUTPUT ${SYS}/icon0.png ${SYS}/livearea/contents/bg.png ${SYS}/livearea/contents/startup.png
    COMMAND python3 ${ROOT}/tools/vita_sce_sys.py ${SYS} "${IGN_DISC}"
    DEPENDS ${ROOT}/tools/vita_sce_sys.py
    COMMENT "LiveArea art"
    VERBATIM)
add_custom_target(ignition_sce_sys DEPENDS ${SYS}/icon0.png)

# Compiled shaders, once there are some: packaged as app0:shaders/*.gxp.
# Without them the game compiles its own on the device (rnd_gxm.c) and
# saves them to ux0:data/ignition/shaders/, from where they are copied
# here.
file(GLOB GXP CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.gxp)
set(GXP_FILES)
foreach(g ${GXP})
    get_filename_component(n ${g} NAME)
    list(APPEND GXP_FILES FILE ${g} shaders/${n})
endforeach()

vita_create_self(eboot.bin ignition UNSAFE)
vita_create_vpk(ignition.vpk IGNT00001 eboot.bin
    VERSION 01.00
    NAME "Ignition"
    FILE ${SYS}/icon0.png sce_sys/icon0.png
         ${SYS}/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
         ${SYS}/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
         ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
         ${GXP_FILES}
)
add_dependencies(ignition.vpk-vpk ignition_sce_sys)
