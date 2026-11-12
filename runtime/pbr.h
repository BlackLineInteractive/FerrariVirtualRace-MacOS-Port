// Physically based replacement for the game's car material (see pbr.cpp).
#pragma once
#include <cstdint>
#include <string>

// rt: also write the reflection mask (colour attachments 2 and 3, see rtm_mask_textures)
void pbr_configure(bool on, bool rt = false);
bool pbr_writes_mask();
// For a car pixel shader (by bytecode hash): the replacement main() body and helper functions to
// add to the translated shader. False when the shader is not replaced.
bool pbr_car_shader(uint64_t hash, std::string &funcs, std::string &main_body);
