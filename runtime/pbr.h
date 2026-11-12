// Physically based replacement for the game's car material (see pbr.cpp).
#pragma once
#include <cstdint>
#include <string>

void pbr_configure(bool on);
// For a car pixel shader (by bytecode hash): the replacement main() body and helper functions to
// add to the translated shader. False when the shader is not replaced.
bool pbr_car_shader(uint64_t hash, std::string &funcs, std::string &main_body);
