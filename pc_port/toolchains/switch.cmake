# Switch/NX toolchain for CMake
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=toolchains/switch.cmake ..

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# devkitPro paths
if(NOT DEFINED DEVKITPRO)
    if(DEFINED ENV{DEVKITPRO})
        set(DEVKITPRO $ENV{DEVKITPRO})
    else()
        # Try Windows default path
        if(EXISTS "C:/devkitPro")
            set(DEVKITPRO "C:/devkitPro")
        else()
            message(FATAL_ERROR "DEVKITPRO not set. Install devkitPro or set env var DEVKITPRO")
        endif()
    endif()
endif()

set(DEVKITARM ${DEVKITPRO}/devkitA64)
set(LIBNX ${DEVKITPRO}/libnx)

message(STATUS "Using devkitPro at: ${DEVKITPRO}")
message(STATUS "Using libnx at: ${LIBNX}")

# Compilers
set(CMAKE_C_COMPILER ${DEVKITARM}/bin/aarch64-none-elf-gcc CACHE PATH "C compiler")
set(CMAKE_CXX_COMPILER ${DEVKITARM}/bin/aarch64-none-elf-g++ CACHE PATH "C++ compiler")
set(CMAKE_AR ${DEVKITARM}/bin/aarch64-none-elf-ar CACHE PATH "Archiver")
set(CMAKE_RANLIB ${DEVKITARM}/bin/aarch64-none-elf-ranlib CACHE PATH "Ranlib")

# Compiler flags for ARM64 (Cortex-A57 in Switch)
set(CMAKE_C_FLAGS "-march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ffunction-sections -fdata-sections" CACHE STRING "C flags")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS}" CACHE STRING "C++ flags")
set(CMAKE_EXE_LINKER_FLAGS "-specs=${LIBNX}/switch.specs -Wl,--gc-sections -L${LIBNX}/lib -L${DEVKITPRO}/portlibs/switch/lib" CACHE STRING "Linker flags")

# Add include directories globally
include_directories(${DEVKITPRO}/portlibs/switch/include)
include_directories(${DEVKITPRO}/portlibs/switch/include/SDL2)

# Find packages configuration
set(CMAKE_FIND_ROOT_PATH ${DEVKITPRO} ${LIBNX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Explicitly set library paths for Switch
set(SDL2_INCLUDE_DIR ${DEVKITPRO}/portlibs/switch/include CACHE PATH "SDL2 include")
set(SDL2_LIBRARY ${DEVKITPRO}/portlibs/switch/lib/libSDL2.a CACHE FILEPATH "SDL2 library")

set(OPENAL_INCLUDE_DIR ${DEVKITPRO}/portlibs/switch/include CACHE PATH "OpenAL include")
set(OPENAL_LIBRARY ${DEVKITPRO}/portlibs/switch/lib/libopenal.a CACHE FILEPATH "OpenAL library")

set(JPEG_INCLUDE_DIR ${DEVKITPRO}/portlibs/switch/include CACHE PATH "libjpeg include")
set(JPEG_LIBRARY ${DEVKITPRO}/portlibs/switch/lib/libjpeg.a CACHE FILEPATH "libjpeg library")

# OpenGL ES for Switch (Mesa)
set(OPENGL_INCLUDE_DIR ${DEVKITPRO}/portlibs/switch/include CACHE PATH "OpenGL ES include")
set(OPENGL_egl_LIBRARY ${DEVKITPRO}/portlibs/switch/lib/libEGL.a CACHE FILEPATH "EGL library")
set(OPENGL_glesv2_LIBRARY ${DEVKITPRO}/portlibs/switch/lib/libGLESv2.a CACHE FILEPATH "GLESv2 library")
set(DRM_NOUVEAU_LIBRARY ${DEVKITPRO}/portlibs/switch/lib/libdrm_nouveau.a CACHE FILEPATH "DRM nouveau library")
set(GLAPI_LIBRARY ${DEVKITPRO}/portlibs/switch/lib/libglapi.a CACHE FILEPATH "GLAPI library")

# Add libnx library search path (the nx library is referenced as -lnx)
link_directories(${LIBNX}/lib)

# Platform-specific definitions
add_compile_definitions(
    __SWITCH__
    __LIBNX__
    RENDERER_OGLES
    OGLES_VERSION=3
)

message(STATUS "Switch toolchain configured successfully")

# Override Windows-specific linker flags that break cross-compilation
set(CMAKE_EXE_LINKER_FLAGS_INIT "")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "")
set(CMAKE_STATIC_LINKER_FLAGS_INIT "")
