set(SDL2_INCLUDE_DIR /c/devkitPro/portlibs/switch/include/SDL2)
set(SDL2_LIBRARY /c/devkitPro/portlibs/switch/lib/libSDL2.a)
set(SDL2main_LIBRARY /c/devkitPro/portlibs/switch/lib/libSDL2main.a)

# Create the target with full path
add_library(SDL2::SDL2 UNKNOWN IMPORTED)
set_target_properties(SDL2::SDL2 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR}"
    IMPORTED_LOCATION "${SDL2_LIBRARY}"
)

set(SDL2_FOUND TRUE)
set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
set(SDL2_LIBRARIES ${SDL2_LIBRARY} ${SDL2main_LIBRARY})
