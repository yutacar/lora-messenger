/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/radio/linux_cap_lora_1262_radio.h"

#include "ports/datagram_transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace lora::adapters::radio {
namespace {

constexpr std::uint8_t kPi4ioAddress = 0x43U;
constexpr std::uint8_t kPi4ioDirection = 0x03U;
constexpr std::uint8_t kPi4ioOutput = 0x05U;
constexpr std::uint8_t kPi4ioHighImpedance = 0x07U;
constexpr std::uint8_t kPi4ioDeviceId = 0x01U;
constexpr std::uint8_t kAntennaSwitchBit = 0x01U;

constexpr std::uint8_t kSetStandby = 0x80U;
constexpr std::uint8_t kSetRx = 0x82U;
constexpr std::uint8_t kSetTx = 0x83U;
constexpr std::uint8_t kSetSleep = 0x84U;
constexpr std::uint8_t kSetRfFrequency = 0x86U;
constexpr std::uint8_t kCalibrate = 0x89U;
constexpr std::uint8_t kSetPacketType = 0x8AU;
constexpr std::uint8_t kSetModulationParams = 0x8BU;
constexpr std::uint8_t kSetPacketParams = 0x8CU;
constexpr std::uint8_t kSetTxParams = 0x8EU;
constexpr std::uint8_t kSetBufferBaseAddress = 0x8FU;
constexpr std::uint8_t kSetPaConfig = 0x95U;
constexpr std::uint8_t kSetRegulatorMode = 0x96U;
constexpr std::uint8_t kSetDio3TcxoCtrl = 0x97U;
constexpr std::uint8_t kCalibrateImage = 0x98U;
constexpr std::uint8_t kSetDioIrqParams = 0x08U;
constexpr std::uint8_t kClearIrqStatus = 0x02U;
constexpr std::uint8_t kWriteRegister = 0x0DU;
constexpr std::uint8_t kWriteBuffer = 0x0EU;
constexpr std::uint8_t kGetIrqStatus = 0x12U;
constexpr std::uint8_t kGetRxBufferStatus = 0x13U;
constexpr std::uint8_t kGetRssiInst = 0x15U;
constexpr std::uint8_t kReadBuffer = 0x1EU;

constexpr std::uint16_t kIrqTxDone = 0x0001U;
constexpr std::uint16_t kIrqRxDone = 0x0002U;
constexpr std::uint16_t kIrqPreambleDetected = 0x0004U;
constexpr std::uint16_t kIrqHeaderValid = 0x0010U;
constexpr std::uint16_t kIrqHeaderError = 0x0020U;
constexpr std::uint16_t kIrqCrcError = 0x0040U;
constexpr std::uint16_t kIrqTimeout = 0x0200U;
constexpr std::uint16_t kReceiveIrqs =
    kIrqRxDone | kIrqPreambleDetected | kIrqHeaderValid |
    kIrqHeaderError | kIrqCrcError | kIrqTimeout;
constexpr std::uint16_t kTransmitIrqs =
    kIrqTxDone | kIrqTimeout;

constexpr std::uint16_t kLoRaSyncWordRegister = 0x0740U;
constexpr std::uint16_t kOcpRegister = 0x08E7U;
constexpr std::uint8_t kLoRaPacketType = 0x01U;
constexpr std::uint8_t kBandwidth125Khz = 0x04U;
constexpr std::uint8_t kExplicitHeader = 0x00U;
constexpr std::uint8_t kCrcEnabled = 0x01U;
constexpr std::uint8_t kStandardIq = 0x00U;
constexpr std::uint8_t kStandbyRc = 0x00U;
constexpr std::uint8_t kRegulatorDcdc = 0x01U;
constexpr std::uint8_t kTcxo3Volts = 0x06U;
constexpr std::uint32_t kTcxoDelayUnits = 320U; // 5 ms / 15.625 us
constexpr std::uint32_t kTxTimeoutUnits = 320'000U; // 5 seconds
constexpr std::uint32_t kBusyTimeoutMs = 100U;

int open_flags(int base) noexcept {
#ifdef O_CLOEXEC
    return base | O_CLOEXEC;
#else
    return base;
#endif
}

bool close_descriptor(int& descriptor) noexcept {
    if (descriptor < 0) {
        return true;
    }
    const int closing = descriptor;
    descriptor = -1;
    while (::close(closing) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

bool gpio_get(int descriptor, bool& high) noexcept {
    gpiohandle_data data{};
    if (::ioctl(
            descriptor, GPIOHANDLE_GET_LINE_VALUES_IOCTL,
            &data) != 0) {
        return false;
    }
    high = data.values[0] != 0U;
    return true;
}

bool gpio_set(int descriptor, bool high) noexcept {
    gpiohandle_data data{};
    data.values[0] = high ? 1U : 0U;
    return ::ioctl(
               descriptor, GPIOHANDLE_SET_LINE_VALUES_IOCTL,
               &data) == 0;
}

int request_gpio(
    int chip_descriptor, std::uint32_t offset, bool output,
    bool initial_high, const char* consumer) noexcept {
    gpiohandle_request request{};
    request.lineoffsets[0] = offset;
    request.flags =
        output ? GPIOHANDLE_REQUEST_OUTPUT : GPIOHANDLE_REQUEST_INPUT;
    request.default_values[0] = initial_high ? 1U : 0U;
    request.lines = 1U;
    std::strncpy(
        request.consumer_label, consumer,
        sizeof(request.consumer_label) - 1U);
    if (::ioctl(
            chip_descriptor, GPIO_GET_LINEHANDLE_IOCTL,
            &request) != 0) {
        return -1;
    }
    return request.fd;
}

} // namespace

bool LinuxCapLora1262Config::valid() const noexcept {
    return !spi_device.empty() && !gpio_chip.empty() &&
           !i2c_device.empty() && spi_speed_hz > 0U &&
           spi_speed_hz <= 10'000'000U &&
           reset_gpio != irq_gpio &&
           reset_gpio != busy_gpio &&
           irq_gpio != busy_gpio &&
           listen_before_talk_threshold_dbm >= -127 &&
           listen_before_talk_threshold_dbm <= -20 &&
           antenna_confirmed && profile.valid();
}

struct LinuxCapLora1262Radio::Impl {
    explicit Impl(LinuxCapLora1262Config value) noexcept
        : config(std::move(value)) {
        initialize();
    }

    ~Impl() {
        shutdown();
    }

    bool ready() const noexcept {
        return initialized && !closed;
    }

    bool wait_while_busy() noexcept {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(kBusyTimeoutMs);
        for (;;) {
            bool busy = false;
            if (!gpio_get(busy_descriptor, busy)) {
                return false;
            }
            if (!busy) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(
                std::chrono::microseconds(100));
        }
    }

    bool spi_transfer(
        const std::uint8_t* transmit, std::uint8_t* receive,
        std::size_t size) noexcept {
        if (spi_descriptor < 0 || !transmit || size == 0U ||
            size > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        spi_ioc_transfer transfer{};
        transfer.tx_buf =
            static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(transmit));
        transfer.rx_buf =
            static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(receive));
        transfer.len = static_cast<std::uint32_t>(size);
        transfer.speed_hz = config.spi_speed_hz;
        transfer.bits_per_word = 8U;
        return ::ioctl(
                   spi_descriptor, SPI_IOC_MESSAGE(1),
                   &transfer) >= 1;
    }

    bool write_command(
        std::uint8_t opcode, const std::uint8_t* data,
        std::size_t size) noexcept {
        if (!wait_while_busy() ||
            size > ports::kMaximumDatagramBytes + 4U) {
            return false;
        }
        std::array<std::uint8_t, ports::kMaximumDatagramBytes + 6U>
            transmit{};
        transmit[0] = opcode;
        if (size > 0U) {
            if (!data) {
                return false;
            }
            std::copy_n(data, size, transmit.begin() + 1);
        }
        if (!spi_transfer(transmit.data(), nullptr, size + 1U)) {
            return false;
        }
        return opcode == kSetSleep || wait_while_busy();
    }

    bool read_command(
        std::uint8_t opcode, const std::uint8_t* arguments,
        std::size_t argument_size, std::uint8_t* destination,
        std::size_t destination_size) noexcept {
        if (!wait_while_busy() || !destination ||
            destination_size == 0U ||
            argument_size + destination_size + 2U >
                ports::kMaximumDatagramBytes + 6U) {
            return false;
        }
        std::array<std::uint8_t, ports::kMaximumDatagramBytes + 6U>
            transmit{};
        std::array<std::uint8_t, ports::kMaximumDatagramBytes + 6U>
            receive{};
        transmit[0] = opcode;
        if (argument_size > 0U) {
            if (!arguments) {
                return false;
            }
            std::copy_n(
                arguments, argument_size, transmit.begin() + 1);
        }
        const auto data_offset = argument_size + 2U;
        const auto transfer_size = data_offset + destination_size;
        if (!spi_transfer(
                transmit.data(), receive.data(), transfer_size) ||
            !wait_while_busy()) {
            return false;
        }
        std::copy_n(
            receive.begin() +
                static_cast<std::ptrdiff_t>(data_offset),
            destination_size, destination);
        return true;
    }

    bool write_register(
        std::uint16_t address, const std::uint8_t* data,
        std::size_t size) noexcept {
        if (!data || size == 0U ||
            size > ports::kMaximumDatagramBytes) {
            return false;
        }
        std::array<std::uint8_t, ports::kMaximumDatagramBytes + 2U>
            arguments{};
        arguments[0] =
            static_cast<std::uint8_t>(address >> 8U);
        arguments[1] =
            static_cast<std::uint8_t>(address & 0xffU);
        std::copy_n(data, size, arguments.begin() + 2);
        return write_command(
            kWriteRegister, arguments.data(), size + 2U);
    }

    bool i2c_read_register(
        std::uint8_t address, std::uint8_t& value) noexcept {
        i2c_msg messages[2]{};
        messages[0].addr = kPi4ioAddress;
        messages[0].flags = 0U;
        messages[0].len = 1U;
        messages[0].buf = &address;
        messages[1].addr = kPi4ioAddress;
        messages[1].flags = I2C_M_RD;
        messages[1].len = 1U;
        messages[1].buf = &value;
        i2c_rdwr_ioctl_data transfer{};
        transfer.msgs = messages;
        transfer.nmsgs = 2U;
        return ::ioctl(
                   i2c_descriptor, I2C_RDWR, &transfer) == 2;
    }

    bool i2c_write_register(
        std::uint8_t address, std::uint8_t value) noexcept {
        std::uint8_t data[2]{address, value};
        i2c_msg message{};
        message.addr = kPi4ioAddress;
        message.flags = 0U;
        message.len = 2U;
        message.buf = data;
        i2c_rdwr_ioctl_data transfer{};
        transfer.msgs = &message;
        transfer.nmsgs = 1U;
        return ::ioctl(
                   i2c_descriptor, I2C_RDWR, &transfer) == 1;
    }

    bool update_i2c_register(
        std::uint8_t address, std::uint8_t set_mask,
        std::uint8_t clear_mask) noexcept {
        std::uint8_t value = 0U;
        if (!i2c_read_register(address, value)) {
            return false;
        }
        value = static_cast<std::uint8_t>(
            (value | set_mask) &
            static_cast<std::uint8_t>(~clear_mask));
        return i2c_write_register(address, value);
    }

    bool set_antenna_switch(bool enabled) noexcept {
        if (i2c_descriptor < 0) {
            return false;
        }
        if (enabled) {
            std::uint8_t device_id = 0U;
            if (!i2c_read_register(
                    kPi4ioDeviceId, device_id) ||
                device_id == 0U ||
                !update_i2c_register(
                    kPi4ioDirection,
                    kAntennaSwitchBit, 0U) ||
                !update_i2c_register(
                    kPi4ioHighImpedance,
                    0U, kAntennaSwitchBit) ||
                !update_i2c_register(
                    kPi4ioOutput,
                    kAntennaSwitchBit, 0U)) {
                return false;
            }
            antenna_switch_enabled = true;
            return true;
        }
        const bool result =
            update_i2c_register(
                kPi4ioOutput, 0U, kAntennaSwitchBit);
        antenna_switch_enabled = false;
        return result;
    }

    bool reset_radio() noexcept {
        if (!gpio_set(reset_descriptor, false)) {
            return false;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(2));
        if (!gpio_set(reset_descriptor, true)) {
            return false;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(10));
        return wait_while_busy();
    }

    bool set_irq_mask(std::uint16_t mask) noexcept {
        const std::uint8_t arguments[]{
            static_cast<std::uint8_t>(mask >> 8U),
            static_cast<std::uint8_t>(mask & 0xffU),
            static_cast<std::uint8_t>(mask >> 8U),
            static_cast<std::uint8_t>(mask & 0xffU),
            0U, 0U, 0U, 0U,
        };
        return write_command(
            kSetDioIrqParams, arguments, sizeof(arguments));
    }

    bool clear_irq(std::uint16_t mask = 0xffffU) noexcept {
        const std::uint8_t arguments[]{
            static_cast<std::uint8_t>(mask >> 8U),
            static_cast<std::uint8_t>(mask & 0xffU),
        };
        return write_command(
            kClearIrqStatus, arguments, sizeof(arguments));
    }

    bool read_irq(std::uint16_t& mask) noexcept {
        std::uint8_t data[2]{};
        if (!read_command(
                kGetIrqStatus, nullptr, 0U, data, sizeof(data))) {
            return false;
        }
        mask = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data[0]) << 8U |
            static_cast<std::uint16_t>(data[1]));
        return true;
    }

    bool set_packet_params(std::size_t payload_size) noexcept {
        if (payload_size == 0U ||
            payload_size > ports::kMaximumDatagramBytes) {
            return false;
        }
        const std::uint8_t arguments[]{
            static_cast<std::uint8_t>(
                config.profile.preamble_symbols >> 8U),
            static_cast<std::uint8_t>(
                config.profile.preamble_symbols & 0xffU),
            kExplicitHeader,
            static_cast<std::uint8_t>(payload_size),
            kCrcEnabled,
            kStandardIq,
        };
        return write_command(
            kSetPacketParams, arguments, sizeof(arguments));
    }

    bool start_receive() noexcept {
        if (!set_packet_params(
                ports::kMaximumDatagramBytes) ||
            !clear_irq() ||
            !set_irq_mask(kReceiveIrqs)) {
            return false;
        }
        const std::uint8_t continuous[]{0xffU, 0xffU, 0xffU};
        if (!write_command(
                kSetRx, continuous, sizeof(continuous))) {
            return false;
        }
        receiving = true;
        transmitting = false;
        return true;
    }

    bool configure_radio() noexcept {
        const std::uint8_t standby[]{kStandbyRc};
        const std::uint8_t regulator[]{kRegulatorDcdc};
        const std::uint8_t tcxo[]{
            kTcxo3Volts,
            static_cast<std::uint8_t>(
                kTcxoDelayUnits >> 16U),
            static_cast<std::uint8_t>(
                kTcxoDelayUnits >> 8U),
            static_cast<std::uint8_t>(kTcxoDelayUnits),
        };
        const std::uint8_t calibrate[]{0x7fU};
        const std::uint8_t image[]{0xe1U, 0xe9U};
        const std::uint8_t packet_type[]{kLoRaPacketType};
        const std::uint8_t buffer_base[]{0U, 0U};
        if (!write_command(
                kSetStandby, standby, sizeof(standby)) ||
            !write_command(
                kSetRegulatorMode, regulator,
                sizeof(regulator)) ||
            !write_command(
                kSetDio3TcxoCtrl, tcxo, sizeof(tcxo)) ||
            !write_command(
                kCalibrate, calibrate, sizeof(calibrate)) ||
            !write_command(
                kCalibrateImage, image, sizeof(image)) ||
            !write_command(
                kSetPacketType, packet_type,
                sizeof(packet_type)) ||
            !write_command(
                kSetBufferBaseAddress, buffer_base,
                sizeof(buffer_base))) {
            return false;
        }

        const std::uint64_t frequency_word =
            (static_cast<std::uint64_t>(
                 config.profile.frequency_hz)
             << 25U) /
            32'000'000ULL;
        const std::uint8_t frequency[]{
            static_cast<std::uint8_t>(frequency_word >> 24U),
            static_cast<std::uint8_t>(frequency_word >> 16U),
            static_cast<std::uint8_t>(frequency_word >> 8U),
            static_cast<std::uint8_t>(frequency_word),
        };
        const std::uint8_t modulation[]{
            config.profile.spreading_factor,
            kBandwidth125Khz,
            static_cast<std::uint8_t>(
                config.profile.coding_rate_denominator - 4U),
            static_cast<std::uint8_t>(
                config.profile.spreading_factor >= 11U ? 1U : 0U),
        };
        const std::uint8_t pa_config[]{
            0x04U, 0x07U, 0x00U, 0x01U};
        const std::uint8_t tx_params[]{
            config.profile.transmit_power_dbm, 0x04U};
        const std::uint8_t ocp[]{0x38U};
        const std::uint8_t sync_word[]{
            static_cast<std::uint8_t>(
                (config.profile.sync_word & 0xf0U) | 0x04U),
            static_cast<std::uint8_t>(
                (config.profile.sync_word << 4U) | 0x04U),
        };
        return write_command(
                   kSetRfFrequency, frequency,
                   sizeof(frequency)) &&
               write_command(
                   kSetModulationParams, modulation,
                   sizeof(modulation)) &&
               write_command(
                   kSetPaConfig, pa_config,
                   sizeof(pa_config)) &&
               write_command(
                   kSetTxParams, tx_params,
                   sizeof(tx_params)) &&
               write_register(
                   kOcpRegister, ocp, sizeof(ocp)) &&
               write_register(
                   kLoRaSyncWordRegister, sync_word,
                   sizeof(sync_word)) &&
               start_receive();
    }

    void initialize() noexcept {
        if (!config.valid()) {
            shutdown();
            return;
        }
        spi_descriptor =
            ::open(config.spi_device.c_str(), open_flags(O_RDWR));
        i2c_descriptor =
            ::open(config.i2c_device.c_str(), open_flags(O_RDWR));
        const int gpio_chip_descriptor =
            ::open(config.gpio_chip.c_str(), open_flags(O_RDONLY));
        if (spi_descriptor < 0 || i2c_descriptor < 0 ||
            gpio_chip_descriptor < 0) {
            int temporary = gpio_chip_descriptor;
            static_cast<void>(close_descriptor(temporary));
            shutdown();
            return;
        }

        reset_descriptor = request_gpio(
            gpio_chip_descriptor, config.reset_gpio,
            true, true, "lora-reset");
        irq_descriptor = request_gpio(
            gpio_chip_descriptor, config.irq_gpio,
            false, false, "lora-irq");
        busy_descriptor = request_gpio(
            gpio_chip_descriptor, config.busy_gpio,
            false, false, "lora-busy");
        int temporary = gpio_chip_descriptor;
        static_cast<void>(close_descriptor(temporary));
        if (reset_descriptor < 0 || irq_descriptor < 0 ||
            busy_descriptor < 0) {
            shutdown();
            return;
        }

        std::uint8_t mode = SPI_MODE_0;
        std::uint8_t bits = 8U;
        std::uint32_t speed = config.spi_speed_hz;
        if (::ioctl(
                spi_descriptor, SPI_IOC_WR_MODE, &mode) != 0 ||
            ::ioctl(
                spi_descriptor, SPI_IOC_WR_BITS_PER_WORD,
                &bits) != 0 ||
            ::ioctl(
                spi_descriptor, SPI_IOC_WR_MAX_SPEED_HZ,
                &speed) != 0 ||
            !set_antenna_switch(true) ||
            !reset_radio() ||
            !configure_radio()) {
            shutdown();
            return;
        }
        initialized = true;
        closed = false;
    }

    RadioStartStatus try_start_transmit(
        const std::uint8_t* data, std::size_t size) noexcept {
        if (!ready() || !data || size == 0U ||
            size > ports::kMaximumDatagramBytes) {
            return RadioStartStatus::Failed;
        }
        if (transmitting) {
            return RadioStartStatus::Busy;
        }

        bool irq_high = false;
        std::uint16_t irq = 0U;
        if (!gpio_get(irq_descriptor, irq_high) ||
            !read_irq(irq)) {
            return RadioStartStatus::Failed;
        }
        if (irq_high ||
            (irq & (kIrqPreambleDetected | kIrqHeaderValid |
                    kIrqRxDone)) != 0U) {
            return RadioStartStatus::Busy;
        }
        std::uint8_t raw_rssi = 0U;
        if (!read_command(
                kGetRssiInst, nullptr, 0U, &raw_rssi, 1U)) {
            return RadioStartStatus::Failed;
        }
        const auto rssi_dbm =
            -static_cast<std::int16_t>(raw_rssi) / 2;
        if (rssi_dbm >=
            config.listen_before_talk_threshold_dbm) {
            return RadioStartStatus::Busy;
        }

        const std::uint8_t standby[]{kStandbyRc};
        if (!write_command(
                kSetStandby, standby, sizeof(standby)) ||
            !set_packet_params(size)) {
            return RadioStartStatus::Failed;
        }
        std::array<std::uint8_t, ports::kMaximumDatagramBytes + 1U>
            buffer{};
        buffer[0] = 0U;
        std::copy_n(data, size, buffer.begin() + 1);
        if (!write_command(
                kWriteBuffer, buffer.data(), size + 1U) ||
            !clear_irq() ||
            !set_irq_mask(kTransmitIrqs)) {
            return RadioStartStatus::Failed;
        }
        const std::uint8_t timeout[]{
            static_cast<std::uint8_t>(kTxTimeoutUnits >> 16U),
            static_cast<std::uint8_t>(kTxTimeoutUnits >> 8U),
            static_cast<std::uint8_t>(kTxTimeoutUnits),
        };
        if (!write_command(kSetTx, timeout, sizeof(timeout))) {
            return RadioStartStatus::Failed;
        }
        transmitting = true;
        receiving = false;
        return RadioStartStatus::Started;
    }

    RadioPollResult poll(
        std::uint8_t* destination,
        std::size_t destination_capacity) noexcept {
        if (!ready() || !destination ||
            destination_capacity == 0U) {
            return {RadioPollStatus::Failed, 0U};
        }
        bool irq_high = false;
        if (!gpio_get(irq_descriptor, irq_high)) {
            return {RadioPollStatus::Failed, 0U};
        }
        if (!irq_high) {
            return {};
        }

        std::uint16_t irq = 0U;
        if (!read_irq(irq)) {
            return {RadioPollStatus::Failed, 0U};
        }
        if ((irq & kIrqTimeout) != 0U && transmitting) {
            return {RadioPollStatus::Failed, 0U};
        }
        if ((irq & kIrqTimeout) != 0U) {
            if (!clear_irq(irq) || !start_receive()) {
                return {RadioPollStatus::Failed, 0U};
            }
            return {};
        }
        if ((irq & kIrqTxDone) != 0U) {
            if (!clear_irq(irq) || !start_receive()) {
                return {RadioPollStatus::Failed, 0U};
            }
            return {RadioPollStatus::TransmitComplete, 0U};
        }
        if ((irq & (kIrqHeaderError | kIrqCrcError)) != 0U) {
            if (!clear_irq(irq) || !start_receive()) {
                return {RadioPollStatus::Failed, 0U};
            }
            return {};
        }
        if ((irq & kIrqRxDone) == 0U) {
            return {};
        }

        std::uint8_t status[2]{};
        if (!read_command(
                kGetRxBufferStatus, nullptr, 0U,
                status, sizeof(status))) {
            return {RadioPollStatus::Failed, 0U};
        }
        const auto payload_size =
            static_cast<std::size_t>(status[0]);
        if (payload_size == 0U ||
            payload_size > destination_capacity) {
            if (!clear_irq(irq) || !start_receive()) {
                return {RadioPollStatus::Failed, 0U};
            }
            return {
                RadioPollStatus::Received,
                payload_size};
        }
        const std::uint8_t offset[]{status[1]};
        if (!read_command(
                kReadBuffer, offset, sizeof(offset),
                destination, payload_size) ||
            !clear_irq(irq) ||
            !start_receive()) {
            return {RadioPollStatus::Failed, 0U};
        }
        return {RadioPollStatus::Received, payload_size};
    }

    void shutdown() noexcept {
        if (closed) {
            return;
        }
        if (spi_descriptor >= 0 && busy_descriptor >= 0 &&
            initialized) {
            const std::uint8_t sleep[]{0x00U};
            static_cast<void>(
                write_command(kSetSleep, sleep, sizeof(sleep)));
        }
        if (antenna_switch_enabled) {
            static_cast<void>(set_antenna_switch(false));
        }
        initialized = false;
        receiving = false;
        transmitting = false;
        closed = true;
        static_cast<void>(close_descriptor(reset_descriptor));
        static_cast<void>(close_descriptor(irq_descriptor));
        static_cast<void>(close_descriptor(busy_descriptor));
        static_cast<void>(close_descriptor(spi_descriptor));
        static_cast<void>(close_descriptor(i2c_descriptor));
    }

    LinuxCapLora1262Config config;
    int spi_descriptor{-1};
    int i2c_descriptor{-1};
    int reset_descriptor{-1};
    int irq_descriptor{-1};
    int busy_descriptor{-1};
    bool antenna_switch_enabled{false};
    bool receiving{false};
    bool transmitting{false};
    bool initialized{false};
    bool closed{false};
};

LinuxCapLora1262Radio::LinuxCapLora1262Radio(
    LinuxCapLora1262Config config) noexcept
    : impl_(std::make_unique<Impl>(std::move(config))) {}

LinuxCapLora1262Radio::~LinuxCapLora1262Radio() = default;

bool LinuxCapLora1262Radio::ready() const noexcept {
    return impl_ && impl_->ready();
}

RadioStartStatus LinuxCapLora1262Radio::try_start_transmit(
    const std::uint8_t* data, std::size_t size) noexcept {
    return impl_
        ? impl_->try_start_transmit(data, size)
        : RadioStartStatus::Failed;
}

RadioPollResult LinuxCapLora1262Radio::poll(
    std::uint8_t* destination,
    std::size_t destination_capacity) noexcept {
    return impl_
        ? impl_->poll(destination, destination_capacity)
        : RadioPollResult{RadioPollStatus::Failed, 0U};
}

void LinuxCapLora1262Radio::shutdown() noexcept {
    if (impl_) {
        impl_->shutdown();
    }
}

} // namespace lora::adapters::radio
