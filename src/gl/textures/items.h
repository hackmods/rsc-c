#ifndef _H_ITEM_TEXTURES
#define _H_ITEM_TEXTURES

#include "../../surface.h"

/* item sprites from GL_ITEM_VARIANT_FIRST up are not in the main sprite atlas: they are on cache/textures/items.png */
#define GL_ITEM_VARIANT_FIRST 435
#define GL_ITEM_VARIANT_COUNT 242

extern gl_atlas_position gl_item_atlas_positions[];
#endif
