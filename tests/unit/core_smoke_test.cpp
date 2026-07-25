/*
 * SPDX-License-Identifier: MIT
 */

#include "core/app_metadata.h"

#include <iostream>

int main() {
    const auto& metadata = lora::core::app_metadata();
    if (!lora::core::metadata_is_valid()) {
        std::cerr << "invalid application metadata\n";
        return 1;
    }
    if (metadata.radio_state != lora::core::RadioState::Disabled) {
        std::cerr << "Phase 0 must not enable a radio adapter\n";
        return 2;
    }
    std::cout << metadata.display_name << " core smoke: "
              << metadata.display_width << 'x' << metadata.display_height
              << ", radio disabled\n";
    return 0;
}
