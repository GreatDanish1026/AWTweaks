// Game shader replacement (see shader_replace.cpp).
#pragma once

namespace shader_replace {

enum class Mode { Original, Enhanced, Debug };
enum class Family { Shadow = 0, AO = 1, SSS = 2 };  // [Shaders] ShadowShader, AOShader, SSSShader

struct Stats {
    Mode mode;
    int pipelinesReplaced;  // pipelines created with one of our shaders
    int pipelinesMissed;    // pipelines that still got a game shader
    int variantsReplaced;   // of the game's distinct shaders in this family, how many we've seen
    int variantsTotal;
};

void Install();  // reads awtweaks.ini [Shaders]; call after reshade::register_addon
void Uninstall();
Stats GetStats(Family family);
const char* ModeName(Family family);

}  // namespace shader_replace
