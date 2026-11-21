/* miniz 3.x (MIT, extern/miniz/LICENSE), configured for reading MeleeForge .ssb packages:
 * zip reading + inflate only, and no zlib-compatible names (aurora links zlib). */
#ifndef MF_MINIZ_H
#define MF_MINIZ_H
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_TIME
#include "miniz.h"
#endif
