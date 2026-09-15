#pragma once

#include <cstdint>

namespace qiven::host
{
enum class BrokerInstanceError : std::uint8_t
{
    none,
    already_running,
    identity_failure,
    system_failure,
    platform_unsupported
};

struct BrokerInstanceAcquireResult;

class BrokerInstanceGuard final
{
public:
    BrokerInstanceGuard() noexcept = default;
    ~BrokerInstanceGuard();

    BrokerInstanceGuard(const BrokerInstanceGuard&) = delete;
    BrokerInstanceGuard& operator=(const BrokerInstanceGuard&) = delete;

    BrokerInstanceGuard(BrokerInstanceGuard&& other) noexcept;
    BrokerInstanceGuard& operator=(BrokerInstanceGuard&& other) noexcept;

    explicit operator bool() const noexcept
    {
        return handle_ != 0;
    }

private:
    explicit BrokerInstanceGuard(std::uintptr_t handle) noexcept : handle_(handle)
    {
    }

    void reset() noexcept;

    std::uintptr_t handle_ {};

    friend BrokerInstanceAcquireResult acquire_broker_instance() noexcept;
};

struct BrokerInstanceAcquireResult final
{
    BrokerInstanceError error {};
    std::uint32_t native_error {};
    BrokerInstanceGuard guard {};

    bool ok() const noexcept
    {
        return error == BrokerInstanceError::none;
    }
};

BrokerInstanceAcquireResult acquire_broker_instance() noexcept;
} // namespace qiven::host
