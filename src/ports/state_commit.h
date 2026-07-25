/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

namespace lora::model {
class Identity;
class Timeline;
}

namespace lora::ports {

// Synchronous durability boundary for MessengerState. Implementations must not
// retain the borrowed references. Returning true means the supplied value is
// durable; false means it was not adopted (or its outcome requires recovery).
class IStateCommit {
public:
    virtual ~IStateCommit() = default;

    virtual bool persist_identity(const model::Identity& identity) noexcept = 0;
    virtual bool persist_timeline(const model::Timeline& timeline) noexcept = 0;
};

} // namespace lora::ports
