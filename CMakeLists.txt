cmake_minimum_required(VERSION 3.20)
project(hardtruck2_recomp C)

# Hard Truck 2 (king.exe), statically recompiled with pcrecomp lift32 and
# hosted on pcrecomp's win32hle (Win32 on libc/POSIX/SDL2, no Wine).
#
#   cmake -B build -G Ninja && cmake --build build      # 64-bit, based memory
#   cmake -B build32 -G Ninja -DHT2_FLAT32=ON           # gcc -m32, flat memory
#
# gen/ must exist first: scripts/run_lift.py all (AGENTS.md, "Pipeline").

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(PCRECOMP "${CMAKE_CURRENT_SOURCE_DIR}/../pcrecomp" CACHE PATH "pcrecomp checkout")
option(HT2_FLAT32 "32-bit host, guest VA == host VA (gcc -m32)" OFF)
# 64-bit: the guest's 4 GB is one mapping at this fixed host address, and the
# lift's ADDR(va) adds it as a constant (win32hle README, "flat or based").
# aarch64 handheld kernels are often built with 39-bit user addresses
# (512 GB), where 0x100000000000 (16 TB) cannot be mapped at all; 256 GB sits
# under that, and under where those kernels load a PIE (2/3 of the space).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(_ht2_base_default "0x4000000000ull")
else()
  set(_ht2_base_default "0x100000000000ull")
endif()
set(HT2_MEM_BASE "${_ht2_base_default}" CACHE STRING "host address of guest VA 0 (64-bit)")
# Everything here was written for x86, where char is signed; on ARM it is not.
add_compile_options(-fsigned-char)
set(HT2_GEN_OPT "-O1" CACHE STRING "optimisation for the 3.7M lines of lifted C")

set(RT32 "${PCRECOMP}/runtime/recomp32")
set(HLE  "${PCRECOMP}/runtime/win32hle")
if(NOT EXISTS "${HLE}/win32hle.h")
  message(FATAL_ERROR "pcrecomp not found at ${PCRECOMP} (-DPCRECOMP=...)")
endif()

if(HT2_FLAT32)
  add_compile_options(-m32)
  add_link_options(-m32)
  set(MEM_DEFS RECOMP_FLAT_MEMORY)
  # Gentoo multilib: the 32-bit .pc files live under /usr/lib (LongRoad2 does the same)
  if(NOT DEFINED ENV{PKG_CONFIG_LIBDIR})
    foreach(_d /usr/lib/pkgconfig /usr/lib32/pkgconfig /usr/lib/i386-linux-gnu/pkgconfig)
      if(EXISTS "${_d}")
        set(ENV{PKG_CONFIG_LIBDIR} "${_d}:/usr/share/pkgconfig")
        break()
      endif()
    endforeach()
  endif()
else()
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "the based build is 64-bit; use -DHT2_FLAT32=ON for 32-bit")
  endif()
  set(MEM_DEFS RECOMP_MEM_BASE=${HT2_MEM_BASE})
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)
pkg_check_modules(GLES REQUIRED glesv2)
pkg_check_modules(MPG123 REQUIRED libmpg123)
find_package(Threads REQUIRED)

# ---- the lifted program ----------------------------------------------------
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/gen/recomp_[0-9]*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "gen/ is empty: run scripts/run_lift.py all first")
endif()
add_library(ht2_gen STATIC ${GEN_SOURCES}
            gen/recomp_dispatch.c gen/recomp_imports.c)
target_include_directories(ht2_gen PRIVATE gen "${RT32}")
target_compile_definitions(ht2_gen PRIVATE ${MEM_DEFS})
# Generated code: warnings are noise, and -O2 over 3.7M lines buys little at
# bring-up for a lot of build time. -fno-strict-aliasing: guest memory is
# accessed through every width at once.
target_compile_options(ht2_gen PRIVATE -w ${HT2_GEN_OPT} -fno-strict-aliasing -fwrapv)
set_target_properties(ht2_gen PROPERTIES UNITY_BUILD OFF)

# ---- pcrecomp win32hle -----------------------------------------------------
file(GLOB HLE_SOURCES "${HLE}/*.c")
list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\\.c$")
add_library(win32hle STATIC ${HLE_SOURCES})
target_include_directories(win32hle PUBLIC "${HLE}" "${RT32}" ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})
target_compile_definitions(win32hle PUBLIC ${MEM_DEFS})
target_compile_options(win32hle PRIVATE -O1 -Wno-unused-function -Wno-unused-but-set-variable
                       -fno-strict-aliasing)

# ---- LongRoad2's DirectDraw 7 / Direct3D 7 over GLES (src/gfx/README.md) ---
# ddraw.c and d3d7.c are not listed: src/shims/ddraw7.c and d3d7.c include
# them, to call their static COM methods with the real prototypes.
add_library(ht2_gfx STATIC
  src/gfx/d3d7_cmdbuf.c
  src/gfx/d3d7_shader.c
  src/gfx/d3d7_d3d3_bridge.c
  src/gfx/render.c
  src/gfx/gl_thread.c
  src/gfx/gl_state_cache.c
  src/gfx/activity_sampler.c
)
target_include_directories(ht2_gfx PUBLIC src/gfx ${SDL2_INCLUDE_DIRS} ${GLES_INCLUDE_DIRS})
target_compile_options(ht2_gfx PRIVATE -O2 -Wno-cast-function-type -fno-strict-aliasing)

# ---- this project ----------------------------------------------------------
add_executable(ht2
  src/host/main.c
  src/shims/misc.c
  src/shims/com_bridge.c
  src/shims/ddraw7.c
  src/shims/d3d7.c
  src/shims/gfx_host.c
  src/shims/dinput.c
  src/shims/dplay.c
  src/shims/quartz.c
)
target_include_directories(ht2 PRIVATE src/shims ${MPG123_INCLUDE_DIRS})
target_compile_options(ht2 PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
# the included LongRoad2 sources build as they do in LongRoad2
set_source_files_properties(src/shims/ddraw7.c src/shims/d3d7.c PROPERTIES
                            COMPILE_OPTIONS "-Wno-cast-function-type;-Wno-unused-function;-Wno-comment;-O2")
target_link_directories(ht2 PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS} ${GLES_LIBRARY_DIRS}
                        ${MPG123_LIBRARY_DIRS})
target_link_libraries(ht2 PRIVATE ht2_gfx win32hle ht2_gen win32hle
                      ${SDL2_LIBRARIES} ${SDL2TTF_LIBRARIES} ${GLES_LIBRARIES} ${MPG123_LIBRARIES}
                      Threads::Threads m dl)


