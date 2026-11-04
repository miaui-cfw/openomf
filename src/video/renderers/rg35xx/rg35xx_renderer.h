#ifndef RG35XX_RENDERER_H
#define RG35XX_RENDERER_H

#include "video/renderers/renderer.h"
#include <stdbool.h>

void rg35xx_renderer_set_callbacks(renderer *renderer);

// --stats: a line of frame statistics every ten seconds.
extern bool rg35xx_renderer_stats;

#endif // RG35XX_RENDERER_H
