#pragma once

#include "qiven/host/authority.hpp"
#include "qiven/host/protocol_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace qiven::host
{
inline constexpr std::size_t owner_pipe_max_connections = 4;

enum class LocalPipeError : std::uint8_t
{
    none,
    platform_unsupported,
    identity_failure,
    security_failure,
    create_failure,
    invalid_slot,
    already_connected,
    retirement_required,
    transport_poisoned,
    not_connected,
    not_retiring,
    connect_failure,
    not_available,
    busy,
    random_failure,
    invalid_frame,
    empty_message,
    message_too_large,
    disconnected,
    read_failure,
    write_failure
};

struct LocalPipeResult final
{
    LocalPipeError error {};
    std::uint32_t native_error {};

    bool ok() const noexcept
    {
        return error == LocalPipeError::none;
    }
};

struct LocalPipeAcceptResult final
{
    LocalPipeResult status {};
    BrokerSessionId session {};

    bool ok() const noexcept
    {
        return status.ok();
    }
};

struct OwnerPipeServerCreateResult;
struct OwnerPipeClientConnectResult;
class TestLocalPipeAccess;

class OwnerPipeServer final
{
public:
    OwnerPipeServer() = default;
    ~OwnerPipeServer();

    OwnerPipeServer(const OwnerPipeServer&) = delete;
    OwnerPipeServer& operator=(const OwnerPipeServer&) = delete;
    OwnerPipeServer(OwnerPipeServer&& other) noexcept;
    OwnerPipeServer& operator=(OwnerPipeServer&& other) noexcept;

    LocalPipeAcceptResult accept(std::size_t slot) noexcept;
    LocalPipeResult session(std::size_t slot, BrokerSessionId& output) const noexcept;
    LocalPipeResult read_frame(std::size_t slot, ProtocolFrame& output) noexcept;
    LocalPipeResult write_frame(std::size_t slot, const ProtocolFrame& frame) noexcept;
    LocalPipeResult disconnect(std::size_t slot) noexcept;
    LocalPipeResult retire(std::size_t slot) noexcept;
    void reset() noexcept;

    explicit operator bool() const noexcept
    {
        return handles_[0] != 0;
    }

private:
    enum class SlotState : std::uint8_t
    {
        listening,
        accepting,
        connected,
        retiring,
        poisoned
    };

    explicit OwnerPipeServer(std::array<std::uintptr_t, owner_pipe_max_connections> handles) noexcept :
        handles_(handles)
    {
    }

    LocalPipeResult mark_retiring(std::size_t slot) noexcept;
    LocalPipeResult abandon_unpublished_connection(std::size_t slot) noexcept;

    std::array<std::uintptr_t, owner_pipe_max_connections> handles_ {};
    std::array<BrokerSessionId, owner_pipe_max_connections> sessions_ {};
    std::array<SlotState, owner_pipe_max_connections> states_ {};
    mutable std::mutex state_mutex_;

    friend OwnerPipeServerCreateResult create_owner_pipe_server() noexcept;
    friend class TestLocalPipeAccess;
};

struct OwnerPipeServerCreateResult final
{
    LocalPipeResult status {};
    OwnerPipeServer server {};

    bool ok() const noexcept
    {
        return status.ok();
    }
};

class OwnerPipeClient final
{
public:
    OwnerPipeClient() = default;
    ~OwnerPipeClient();

    OwnerPipeClient(const OwnerPipeClient&) = delete;
    OwnerPipeClient& operator=(const OwnerPipeClient&) = delete;
    OwnerPipeClient(OwnerPipeClient&& other) noexcept;
    OwnerPipeClient& operator=(OwnerPipeClient&& other) noexcept;

    LocalPipeResult read_frame(ProtocolFrame& output) noexcept;
    LocalPipeResult write_frame(const ProtocolFrame& frame) noexcept;
    void reset() noexcept;

    explicit operator bool() const noexcept
    {
        return handle_ != 0;
    }

private:
    explicit OwnerPipeClient(std::uintptr_t handle) noexcept : handle_(handle)
    {
    }

    std::uintptr_t handle_ {};

    friend OwnerPipeClientConnectResult connect_owner_pipe_client() noexcept;
};

struct OwnerPipeClientConnectResult final
{
    LocalPipeResult status {};
    OwnerPipeClient client {};

    bool ok() const noexcept
    {
        return status.ok();
    }
};

OwnerPipeServerCreateResult create_owner_pipe_server() noexcept;
OwnerPipeClientConnectResult connect_owner_pipe_client() noexcept;
} // namespace qiven::host
