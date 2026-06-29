#if defined(__SWITCH__)

#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

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
