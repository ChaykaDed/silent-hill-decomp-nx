file(GLOB_RECURSE PSYCROSS_SRCS_C
	"*.c"
)

file(GLOB_RECURSE PSYCROSS_SRCS_CPP
	"*.cpp"
)

# Exclude incompatible glad source for each platform
if(SH_SWITCH)
    message(STATUS "glad filter: found .c files: ${PSYCROSS_SRCS_C}")
    list(FILTER PSYCROSS_SRCS_C EXCLUDE REGEX ".*glad\\.c$")
    message(STATUS "glad filter: after exclusion: ${PSYCROSS_SRCS_C}")
else()
    message(STATUS "glad_switch filter: found .c files: ${PSYCROSS_SRCS_C}")
    list(FILTER PSYCROSS_SRCS_C EXCLUDE REGEX ".*glad_switch\\.c$")
    message(STATUS "glad_switch filter: after exclusion: ${PSYCROSS_SRCS_C}")
endif()

set_source_files_properties( ${PSYCROSS_SRCS_C} PROPERTIES LANGUAGE C)
set_source_files_properties( ${PSYCROSS_SRCS_CPP} PROPERTIES LANGUAGE CXX)

# C11 uses _Static_assert, but PsyCross headers use C++ style static_assert
# Add compatibility define for C compilation
set_source_files_properties( ${PSYCROSS_SRCS_C} PROPERTIES
    COMPILE_DEFINITIONS "static_assert=_Static_assert"
)

# AppleClang treats an uppercase .C extension as C++ regardless of the LANGUAGE
# property. Force C mode explicitly so -std=gnu11 is accepted.
if(APPLE)
    set_source_files_properties( ${PSYCROSS_SRCS_C} PROPERTIES COMPILE_FLAGS "-x c")
endif()
#set_source_files_properties( "src/gte/PsyX_GTE.cpp" PROPERTIES LANGUAGE C)

add_library(psycross_static STATIC ${PSYCROSS_SRCS_C} ${PSYCROSS_SRCS_CPP})
set_target_properties(psycross_static PROPERTIES OUTPUT_NAME "psycross")
target_include_directories(psycross_static PUBLIC "include" "include/psx")

# On ELF the host executable that links this archive is built with
# -rdynamic (so map overlay .so's can resolve game symbols at dlopen). That
# also exports every default-visibility global, and two TUs here define
# symbols whose names collide with libc and libopenal:
#   - libapi.c implements PSX file I/O as open/read/close/write/lseek/ioctl/
#     rename. Exporting these interposed libc for ALL loaded libraries —
#     Mesa's read() of the GPU's PCI sysfs `vendor` node during GL context
#     creation was dispatched to the game's read() and hung startup.
#   - PsyX_SPUAL.cpp holds alGenEffects/... as global function-pointer
#     variables, which interposed libopenal's EFX functions.
# -rdynamic cannot export hidden-visibility symbols, so compile just these
# TUs hidden. They're still used normally within the module; nothing
# external needs them (the maps link their own copy of this archive).
if(NOT WIN32)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/psx/libapi.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PsyX_SPUAL.cpp"
        PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")
endif()

# For Switch (devkitPro), SDL2 is provided without CMake config files
# Use the pre-set variables from the toolchain
if(DEFINED SDL2_LIBRARY)
    target_link_libraries(psycross_static PRIVATE ${SDL2_LIBRARY})
    target_include_directories(psycross_static PRIVATE ${SDL2_INCLUDE_DIR} ${SDL2_INCLUDE_DIR}/SDL2)
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(psycross_static PRIVATE ${SDL2_LIBRARIES})
    target_include_directories(psycross_static PRIVATE ${SDL2_INCLUDE_DIRS})
endif()

# For Switch (devkitPro), OpenAL is provided without CMake config files
# Use the pre-set variables from the toolchain
if(DEFINED OPENAL_LIBRARY)
    target_link_libraries(psycross_static PRIVATE ${OPENAL_LIBRARY})
    target_include_directories(psycross_static PRIVATE ${OPENAL_INCLUDE_DIR})
else()
    find_package(OpenAL REQUIRED)
    # find_package(OpenAL) exposes different handles depending on how OpenAL was
    # found: the imported target OpenAL::OpenAL (openal-soft's CMake config, or
    # CMake's FindOpenAL in 3.x), a bare `OpenAL` target (some MSYS2 configs), or
    # just the OPENAL_LIBRARY / OPENAL_INCLUDE_DIR variables. Prefer the imported
    # target; fall back so this links on both MinGW and Linux.
    if(TARGET OpenAL::OpenAL)
        target_link_libraries(psycross_static PRIVATE OpenAL::OpenAL)
    elseif(TARGET OpenAL)
        target_link_libraries(psycross_static PRIVATE OpenAL)
    else()
        target_link_libraries(psycross_static PRIVATE ${OPENAL_LIBRARY})
        target_include_directories(psycross_static PRIVATE ${OPENAL_INCLUDE_DIR})
    endif()
endif()
# Guard against an empty OpenAL_SOURCE_DIR (e.g. macOS Homebrew / system
# framework, where find_package uses the imported target, not an in-tree build)
# expanding to a bogus "/include" path.
if(OpenAL_SOURCE_DIR)
    target_include_directories(psycross_static PRIVATE ${OpenAL_BINARY_DIR} ${OpenAL_SOURCE_DIR}/include)
endif()
