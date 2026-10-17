// Diagnostic logging of which game shaders draw (see shader_watch.cpp).
#pragma once

namespace shader_watch {

void Install();  // reads awtweaks.ini [Debug] WatchShaders; call after reshade::register_addon
void Uninstall();

}  // namespace shader_watch
