/*
 * OpenTyrian: Dummy Scaler Definitions
 * * We keep these variables so the "Game Options" menu and Config system
 * don't crash, but the actual scaling is now handled by the GPU in video.c
 */

#include "video_scale.h"
#include "video.h"
#include <string.h>

 // The currently selected scaler index (read by config.c)
uint scaler = 0;

// The list of scalers shown in the menu
const struct Scalers scalers[] =
{
    // We define these so the menu has something to show.
    // The GPU shader handles the actual look, so these are mostly labels now.
    { 320, 200, "GPU: CRT Shader" },
    { 640, 400, "GPU: Sharp Bilinear" },
    { 640, 400, "GPU: Nearest Neighbor" },
};

const uint scalers_count = COUNTOF(scalers);

// Helper used by config.c to load settings
void set_scaler_by_name(const char* name)
{
    for (uint i = 0; i < scalers_count; ++i)
    {
        if (strcmp(name, scalers[i].name) == 0)
        {
            scaler = i;
            break;
        }
    }
}
