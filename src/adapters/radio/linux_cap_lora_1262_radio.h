/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "adapters/radio/cap_lora_1262_radio.h"
#include "adapters/radio/japan_920_radio_policy.h"

#include <cstdint>
#include <memory>
#include <string>

namespace lora::adapters::radio {

struct LinuxCapLora1262Config {
    // CardputerZero EXT uses SPI0 CS1 for the Zero-compatible Cap orientation.
    std::string spi_device{"/dev/spidev0.1"};
    std::string gpio_chip{"/dev/gpiochip0"};
    std::string i2c_device{"/dev/i2c-1"};
    std::uint32_t reset_gpio{26U};
    std::uint32_t irq_gpio{23U};
    std::uint32_t busy_gpio{22U};
    std::uint32_t spi_speed_hz{4'000'000U};
    std::int16_t listen_before_talk_threshold_dbm{-90};
    bool antenna_confirmed{false};
    Japan920Profile profile;

    bool valid() const noexcept;
};

// Linux spidev/GPIO/I2C implementation for the Zero-compatible Cap LoRa-1262.
// Construction is fail-closed. antenna_confirmed must be explicitly true; the
// M5Stack hardware warning says powering/transmitting without the antenna can
// permanently damage the module.
class LinuxCapLora1262Radio final : public ICapLora1262Radio {
public:
    explicit LinuxCapLora1262Radio(
        LinuxCapLora1262Config config) noexcept;
    ~LinuxCapLora1262Radio() override;

    LinuxCapLora1262Radio(const LinuxCapLora1262Radio&) = delete;
    LinuxCapLora1262Radio& operator=(
        const LinuxCapLora1262Radio&) = delete;

    bool ready() const noexcept override;
    RadioStartStatus try_start_transmit(
        const std::uint8_t* data, std::size_t size) noexcept override;
    RadioPollResult poll(
        std::uint8_t* destination,
        std::size_t destination_capacity) noexcept override;
    void shutdown() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lora::adapters::radio
