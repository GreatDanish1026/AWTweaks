// Shadow shader replacement (see shader_replace.cpp).
#pragma once

namespace shader_replace {

enum class Mode { Original, Enhanced, Debug };

struct Stats {
    Mode mode;
    int pipelinesReplaced;  // pipelines created with one of our shaders
    int pipelinesMissed;    // pipelines that still got a game shader
    int variantsReplaced;   // of the game's distinct shadow shaders, how many we've seen
    int variantsTotal;
};

void Install();  // reads awtweaks.ini [Shaders]; call after reshade::register_addon
void Uninstall();
Stats GetStats();
const char* ModeName();

}  // namespace shader_replace
