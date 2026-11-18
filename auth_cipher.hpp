#pragma once
#include <array>
#include <cstdint>

namespace miearphone {

// Computes the 16-byte RCSP/MMA authentication response for a 16-byte challenge.
// Implements function_E21 (8-round custom block cipher + 16-round key schedule).
std::array<uint8_t, 16> auth_response(const uint8_t rnd16[16]);

} // namespace miearphone
