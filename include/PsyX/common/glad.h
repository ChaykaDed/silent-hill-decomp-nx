#if defined(__SWITCH__)

#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
/* GLES 3.2 for GL_CLAMP_TO_BORDER / GL_TEXTURE_BORDER_COLOR (flashlight
 * shadow map). gl32.h pulls in gl31.h + gl3.h, so this is purely additive.
 * Mesa on Switch (switch-mesa 20.1+) exposes GLES 3.2. */
#include <GLES3/gl32.h>

#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA 0x190A
#endif

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif
#ifndef GLAPIENTRY
#define GLAPIENTRY APIENTRY
#endif

typedef void* (* GLADloadproc)(const char *name);

#else
#  include "glad_desktop.h"
#endif
