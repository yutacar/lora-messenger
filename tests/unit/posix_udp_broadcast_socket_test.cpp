/*
 * SPDX-License-Identifier: MIT
 */

#include "adapters/network/posix_udp_broadcast_socket.h"

#include "test_support.h"

#include <net/if.h>

#include <string>
#include <utility>

int main() {
    lora::test::Runner runner;
    using lora::adapters::network::PosixUdpBroadcastConfig;
    using lora::adapters::network::PosixUdpBroadcastSocket;

    runner.run("configuration rejects unsafe port and interface", [&] {
        PosixUdpBroadcastConfig config;
        CHECK(config.valid());
        config.port = 0U;
        CHECK(!config.valid());
        config.port = 1024U;
        config.interface_name.clear();
        CHECK(!config.valid());
        config.interface_name =
            std::string(IFNAMSIZ, 'x');
        CHECK(!config.valid());
    });

    runner.run("invalid construction and close fail safely", [&] {
        PosixUdpBroadcastConfig config;
        config.port = 0U;
        PosixUdpBroadcastSocket socket(std::move(config));
        CHECK(!socket.ready());
        socket.close();
        socket.close();
        CHECK(!socket.ready());
    });

    return runner.finish();
}
