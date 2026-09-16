#include "qiven/host/local_pipe_transport.hpp"

#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <sddl.h>
#endif

namespace qiven::host
{
namespace
{
#if defined(_WIN32)
struct SidResult final
{
    LocalPipeResult status {};
    std::wstring text;
};

SidResult current_user_sid() noexcept
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0)
        return { { LocalPipeError::identity_failure, GetLastError() }, {} };

    DWORD bytes = 0;
    SetLastError(ERROR_SUCCESS);
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    const DWORD size_error = GetLastError();
    if (bytes == 0 || size_error != ERROR_INSUFFICIENT_BUFFER)
    {
        const DWORD error = size_error != ERROR_SUCCESS ? size_error : ERROR_INVALID_DATA;
        CloseHandle(token);
        return { { LocalPipeError::identity_failure, error }, {} };
    }

    void* buffer = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (buffer == nullptr)
    {
        CloseHandle(token);
        return { { LocalPipeError::identity_failure, ERROR_NOT_ENOUGH_MEMORY }, {} };
    }

    if (GetTokenInformation(token, TokenUser, buffer, bytes, &bytes) == 0)
    {
        const DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, buffer);
        CloseHandle(token);
        return { { LocalPipeError::identity_failure, error }, {} };
    }

    const auto* user = static_cast<const TOKEN_USER*>(buffer);
    LPWSTR sid_text = nullptr;
    if (ConvertSidToStringSidW(user->User.Sid, &sid_text) == 0)
    {
        const DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, buffer);
        CloseHandle(token);
        return { { LocalPipeError::identity_failure, error }, {} };
    }

    SidResult result;
    try
    {
        result.text = sid_text;
    }
    catch (...)
    {
        result.status = { LocalPipeError::identity_failure, ERROR_NOT_ENOUGH_MEMORY };
    }

    LocalFree(sid_text);
    HeapFree(GetProcessHeap(), 0, buffer);
    if (CloseHandle(token) == 0 && result.status.ok())
        result.status = { LocalPipeError::identity_failure, GetLastError() };
    return result;
}

struct NameResult final
{
    LocalPipeResult status {};
    std::wstring name;
    std::wstring sid;
};

NameResult owner_pipe_name() noexcept
{
    const SidResult sid = current_user_sid();
    if (!sid.status.ok())
        return { sid.status, {}, {} };

    NameResult result;
    try
    {
        result.name = L"\\\\.\\pipe\\QivenHost.Broker." + sid.text;
        result.sid = sid.text;
    }
    catch (...)
    {
        result.status = { LocalPipeError::identity_failure, ERROR_NOT_ENOUGH_MEMORY };
    }
    return result;
}

LocalPipeResult random_session(BrokerSessionId& output) noexcept
{
    for (unsigned attempt = 0; attempt != 4; ++attempt)
    {
        BrokerSessionId candidate {};
        const NTSTATUS status = BCryptGenRandom(nullptr, candidate.bytes.data(), static_cast<ULONG>(candidate.bytes.size()),
                                                BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0)
            return { LocalPipeError::random_failure, static_cast<std::uint32_t>(status) };

        bool nonzero = false;
        for (const std::uint8_t byte : candidate.bytes)
            nonzero = nonzero || byte != 0;
        if (nonzero)
        {
            output = candidate;
            return {};
        }
    }
    return { LocalPipeError::random_failure, ERROR_INVALID_DATA };
}

bool session_nonzero(const BrokerSessionId& session) noexcept
{
    for (const std::uint8_t byte : session.bytes)
    {
        if (byte != 0)
            return true;
    }
    return false;
}

LocalPipeError disconnected_error(DWORD error) noexcept
{
    switch (error)
    {
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_PIPE_NOT_CONNECTED:
    case ERROR_OPERATION_ABORTED:
        return LocalPipeError::disconnected;
    default:
        return LocalPipeError::read_failure;
    }
}

class OverlappedEvent final
{
public:
    OverlappedEvent() noexcept
    {
        overlap_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~OverlappedEvent()
    {
        if (overlap_.hEvent != nullptr)
            CloseHandle(overlap_.hEvent);
    }

    OverlappedEvent(const OverlappedEvent&) = delete;
    OverlappedEvent& operator=(const OverlappedEvent&) = delete;

    bool valid() const noexcept
    {
        return overlap_.hEvent != nullptr;
    }

    OVERLAPPED& value() noexcept
    {
        return overlap_;
    }

private:
    OVERLAPPED overlap_ {};
};

struct PendingIoResult final
{
    bool timed_out {};
    DWORD error { ERROR_SUCCESS };
    DWORD bytes {};
};

PendingIoResult wait_pending_io(HANDLE handle, OVERLAPPED& overlap, std::uint32_t timeout_ms) noexcept
{
    const DWORD wait = WaitForSingleObject(overlap.hEvent, timeout_ms);
    if (wait == WAIT_OBJECT_0)
    {
        DWORD bytes = 0;
        if (GetOverlappedResult(handle, &overlap, &bytes, FALSE) != 0)
            return { false, ERROR_SUCCESS, bytes };
        return { false, GetLastError(), bytes };
    }

    if (wait == WAIT_TIMEOUT)
    {
        DWORD bytes = 0;
        if (GetOverlappedResult(handle, &overlap, &bytes, FALSE) != 0)
            return { false, ERROR_SUCCESS, bytes };

        const DWORD completion_error = GetLastError();
        if (completion_error != ERROR_IO_INCOMPLETE)
            return { false, completion_error, bytes };

        const BOOL cancelled = CancelIoEx(handle, &overlap);
        const DWORD cancel_error = cancelled == 0 ? GetLastError() : ERROR_SUCCESS;

        // OVERLAPPED is stack-owned. Cancellation is followed by completion drain so the
        // kernel cannot later write through storage that has left scope. The peer wait is
        // bounded by timeout_ms; this drain is kernel-completion safety after cancellation.
        WaitForSingleObject(overlap.hEvent, INFINITE);
        bytes = 0;
        const BOOL completed = GetOverlappedResult(handle, &overlap, &bytes, FALSE);
        const DWORD final_error = completed != 0 ? ERROR_SUCCESS : GetLastError();

        if (cancelled == 0 && cancel_error == ERROR_NOT_FOUND)
            return { false, final_error, bytes };
        return { true, final_error, bytes };
    }

    const DWORD wait_error = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
    CancelIoEx(handle, &overlap);
    WaitForSingleObject(overlap.hEvent, INFINITE);
    DWORD bytes = 0;
    GetOverlappedResult(handle, &overlap, &bytes, FALSE);
    return { false, wait_error, bytes };
}

PendingIoResult read_overlapped(HANDLE handle, void* buffer, DWORD capacity, std::uint32_t timeout_ms) noexcept
{
    OverlappedEvent event;
    if (!event.valid())
        return { false, GetLastError(), 0 };

    DWORD bytes = 0;
    if (ReadFile(handle, buffer, capacity, &bytes, &event.value()) != 0)
        return { false, ERROR_SUCCESS, bytes };

    const DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING)
        return { false, error, bytes };
    return wait_pending_io(handle, event.value(), timeout_ms);
}

PendingIoResult write_overlapped(HANDLE handle, const void* buffer, DWORD size, std::uint32_t timeout_ms) noexcept
{
    OverlappedEvent event;
    if (!event.valid())
        return { false, GetLastError(), 0 };

    DWORD bytes = 0;
    if (WriteFile(handle, buffer, size, &bytes, &event.value()) != 0)
        return { false, ERROR_SUCCESS, bytes };

    const DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING)
        return { false, error, bytes };
    return wait_pending_io(handle, event.value(), timeout_ms);
}
#endif
} // namespace

OwnerPipeServer::~OwnerPipeServer()
{
    reset();
}

OwnerPipeServer::OwnerPipeServer(OwnerPipeServer&& other) noexcept :
    handles_(other.handles_), sessions_(other.sessions_), states_(other.states_)
{
    other.handles_ = {};
    other.sessions_ = {};
    other.states_ = {};
}

OwnerPipeServer& OwnerPipeServer::operator=(OwnerPipeServer&& other) noexcept
{
    if (this != &other)
    {
        reset();
        handles_ = other.handles_;
        sessions_ = other.sessions_;
        states_ = other.states_;
        other.handles_ = {};
        other.sessions_ = {};
        other.states_ = {};
    }
    return *this;
}

LocalPipeResult OwnerPipeServer::mark_retiring(std::size_t slot) noexcept
{
#if !defined(_WIN32)
    (void)slot;
    return { LocalPipeError::platform_unsupported, 0 };
#else
    if (slot >= owner_pipe_max_connections || handles_[slot] == 0)
        return { LocalPipeError::invalid_slot, 0 };

    std::lock_guard lock(state_mutex_);
    if (states_[slot] == SlotState::poisoned)
        return { LocalPipeError::transport_poisoned, 0 };
    if (states_[slot] == SlotState::retiring)
        return {};
    if (states_[slot] != SlotState::connected)
        return { LocalPipeError::not_connected, 0 };

    HANDLE handle = reinterpret_cast<HANDLE>(handles_[slot]);
    if (CancelIoEx(handle, nullptr) == 0)
    {
        const DWORD cancel_error = GetLastError();
        if (cancel_error != ERROR_NOT_FOUND)
        {
            states_[slot] = SlotState::poisoned;
            return { LocalPipeError::transport_poisoned, cancel_error };
        }
    }

    const BOOL disconnected = DisconnectNamedPipe(handle);
    const DWORD error = disconnected == 0 ? GetLastError() : ERROR_SUCCESS;
    if (disconnected != 0 || error == ERROR_PIPE_NOT_CONNECTED)
    {
        states_[slot] = SlotState::retiring;
        return {};
    }

    states_[slot] = SlotState::poisoned;
    return { LocalPipeError::transport_poisoned, error };
#endif
}

LocalPipeResult OwnerPipeServer::abandon_unpublished_connection(std::size_t slot) noexcept
{
#if !defined(_WIN32)
    (void)slot;
    return { LocalPipeError::platform_unsupported, 0 };
#else
    if (slot >= owner_pipe_max_connections || handles_[slot] == 0)
        return { LocalPipeError::invalid_slot, 0 };

    std::lock_guard lock(state_mutex_);
    if (states_[slot] == SlotState::poisoned)
        return { LocalPipeError::transport_poisoned, 0 };
    if (states_[slot] != SlotState::accepting)
        return { LocalPipeError::connect_failure, ERROR_INVALID_STATE };

    HANDLE handle = reinterpret_cast<HANDLE>(handles_[slot]);
    if (CancelIoEx(handle, nullptr) == 0)
    {
        const DWORD cancel_error = GetLastError();
        if (cancel_error != ERROR_NOT_FOUND)
        {
            states_[slot] = SlotState::poisoned;
            return { LocalPipeError::transport_poisoned, cancel_error };
        }
    }

    const BOOL disconnected = DisconnectNamedPipe(handle);
    const DWORD error = disconnected == 0 ? GetLastError() : ERROR_SUCCESS;
    sessions_[slot] = {};
    if (disconnected != 0 || error == ERROR_PIPE_NOT_CONNECTED)
    {
        states_[slot] = SlotState::listening;
        return {};
    }

    states_[slot] = SlotState::poisoned;
    return { LocalPipeError::transport_poisoned, error };
#endif
}

void OwnerPipeServer::reset() noexcept
{
#if defined(_WIN32)
    for (std::size_t slot = 0; slot != owner_pipe_max_connections; ++slot)
    {
        if (handles_[slot] != 0)
        {
            CancelIoEx(reinterpret_cast<HANDLE>(handles_[slot]), nullptr);
            DisconnectNamedPipe(reinterpret_cast<HANDLE>(handles_[slot]));
            CloseHandle(reinterpret_cast<HANDLE>(handles_[slot]));
        }
    }
#endif
    handles_ = {};
    sessions_ = {};
    states_ = {};
}

LocalPipeAcceptResult OwnerPipeServer::accept(std::size_t slot, std::uint32_t timeout_ms) noexcept
{
#if !defined(_WIN32)
    (void)slot;
    (void)timeout_ms;
    return { { LocalPipeError::platform_unsupported, 0 }, {} };
#else
    if (slot >= owner_pipe_max_connections || handles_[slot] == 0)
        return { { LocalPipeError::invalid_slot, 0 }, {} };

    {
        std::lock_guard lock(state_mutex_);
        if (states_[slot] == SlotState::retiring)
            return { { LocalPipeError::retirement_required, 0 }, {} };
        if (states_[slot] == SlotState::poisoned)
            return { { LocalPipeError::transport_poisoned, 0 }, {} };
        if (states_[slot] != SlotState::listening)
            return { { LocalPipeError::already_connected, 0 }, {} };
        states_[slot] = SlotState::accepting;
    }

    HANDLE handle = reinterpret_cast<HANDLE>(handles_[slot]);
    OverlappedEvent event;
    if (!event.valid())
    {
        const DWORD event_error = GetLastError();
        const LocalPipeResult abandoned = abandon_unpublished_connection(slot);
        if (!abandoned.ok())
            return { abandoned, {} };
        return { { LocalPipeError::connect_failure, event_error }, {} };
    }

    bool connected = false;
    BOOL immediate = ConnectNamedPipe(handle, &event.value());
    if (immediate != 0)
    {
        connected = true;
    }
    else
    {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
        {
            connected = true;
        }
        else if (error == ERROR_IO_PENDING)
        {
            const PendingIoResult pending = wait_pending_io(handle, event.value(), timeout_ms);
            if (pending.timed_out)
            {
                const LocalPipeResult abandoned = abandon_unpublished_connection(slot);
                if (!abandoned.ok())
                    return { abandoned, {} };
                return { { LocalPipeError::timeout, WAIT_TIMEOUT }, {} };
            }
            if (pending.error == ERROR_SUCCESS || pending.error == ERROR_PIPE_CONNECTED)
            {
                connected = true;
            }
            else
            {
                const LocalPipeResult abandoned = abandon_unpublished_connection(slot);
                if (!abandoned.ok())
                    return { abandoned, {} };
                return { { pending.error == ERROR_NO_DATA ? LocalPipeError::disconnected : LocalPipeError::connect_failure,
                           pending.error },
                         {} };
            }
        }
        else
        {
            const LocalPipeResult abandoned = abandon_unpublished_connection(slot);
            if (!abandoned.ok())
                return { abandoned, {} };
            return { { error == ERROR_NO_DATA ? LocalPipeError::disconnected : LocalPipeError::connect_failure, error },
                     {} };
        }
    }

    if (!connected)
    {
        const LocalPipeResult abandoned = abandon_unpublished_connection(slot);
        if (!abandoned.ok())
            return { abandoned, {} };
        return { { LocalPipeError::connect_failure, ERROR_GEN_FAILURE }, {} };
    }

    BrokerSessionId session_id {};
    bool unique = false;
    for (unsigned attempt = 0; attempt != 8 && !unique; ++attempt)
    {
        const LocalPipeResult random = random_session(session_id);
        if (!random.ok())
        {
            const LocalPipeResult abandoned = abandon_unpublished_connection(slot);
            if (!abandoned.ok())
                return { abandoned, {} };
            return { random, {} };
        }

        std::lock_guard lock(state_mutex_);
        bool collision = false;
        for (std::size_t active = 0; active != owner_pipe_max_connections; ++active)
        {
            if ((states_[active] == SlotState::connected || states_[active] == SlotState::retiring ||
                 states_[active] == SlotState::poisoned) &&
                sessions_[active] == session_id)
            {
                collision = true;
                break;
            }
        }
        if (!collision)
        {
            sessions_[slot] = session_id;
            states_[slot] = SlotState::connected;
            unique = true;
        }
    }
    if (!unique)
    {
        const LocalPipeResult abandoned = abandon_unpublished_connection(slot);
        if (!abandoned.ok())
            return { abandoned, {} };
        return { { LocalPipeError::random_failure, ERROR_DUP_NAME }, {} };
    }

    return { {}, session_id };
#endif
}

LocalPipeResult OwnerPipeServer::session(std::size_t slot, BrokerSessionId& output) const noexcept
{
#if !defined(_WIN32)
    (void)slot;
    (void)output;
    return { LocalPipeError::platform_unsupported, 0 };
#else
    if (slot >= owner_pipe_max_connections || handles_[slot] == 0)
        return { LocalPipeError::invalid_slot, 0 };

    std::lock_guard lock(state_mutex_);
    const bool published_state = states_[slot] == SlotState::connected || states_[slot] == SlotState::retiring ||
                                 states_[slot] == SlotState::poisoned;
    if (!published_state || !session_nonzero(sessions_[slot]))
        return { LocalPipeError::not_connected, 0 };
    output = sessions_[slot];
    return {};
#endif
}

LocalPipeResult OwnerPipeServer::read_frame(std::size_t slot, ProtocolFrame& output, std::uint32_t timeout_ms) noexcept
{
#if !defined(_WIN32)
    (void)slot;
    (void)output;
    (void)timeout_ms;
    return { LocalPipeError::platform_unsupported, 0 };
#else
    if (slot >= owner_pipe_max_connections || handles_[slot] == 0)
        return { LocalPipeError::invalid_slot, 0 };
    {
        std::lock_guard lock(state_mutex_);
        if (states_[slot] == SlotState::retiring)
            return { LocalPipeError::retirement_required, 0 };
        if (states_[slot] == SlotState::poisoned)
            return { LocalPipeError::transport_poisoned, 0 };
        if (states_[slot] != SlotState::connected)
            return { LocalPipeError::not_connected, 0 };
    }

    ProtocolFrame candidate {};
    const PendingIoResult read = read_overlapped(reinterpret_cast<HANDLE>(handles_[slot]), candidate.bytes.data(),
                                                  static_cast<DWORD>(candidate.bytes.size()), timeout_ms);
    if (read.timed_out)
    {
        const LocalPipeResult retirement = mark_retiring(slot);
        if (!retirement.ok())
            return retirement;
        return { LocalPipeError::timeout, WAIT_TIMEOUT };
    }
    if (read.error != ERROR_SUCCESS)
    {
        const LocalPipeResult retirement = mark_retiring(slot);
        if (!retirement.ok())
            return retirement;
        if (read.error == ERROR_MORE_DATA)
            return { LocalPipeError::message_too_large, read.error };
        return { disconnected_error(read.error), read.error };
    }
    if (read.bytes == 0)
    {
        const LocalPipeResult retirement = mark_retiring(slot);
        if (!retirement.ok())
            return retirement;
        return { LocalPipeError::empty_message, 0 };
    }

    candidate.size = static_cast<std::uint16_t>(read.bytes);
    output = candidate;
    return {};
#endif
}

LocalPipeResult OwnerPipeServer::write_frame(std::size_t slot, const ProtocolFrame& frame, std::uint32_t timeout_ms) noexcept
{
#if !defined(_WIN32)
    (void)slot;
    (void)frame;
    (void)timeout_ms;
    return { LocalPipeError::platform_unsupported, 0 };
#else
    if (slot >= owner_pipe_max_connections || handles_[slot] == 0)
        return { LocalPipeError::invalid_slot, 0 };
    {
        std::lock_guard lock(state_mutex_);
        if (states_[slot] == SlotState::retiring)
            return { LocalPipeError::retirement_required, 0 };
        if (states_[slot] == SlotState::poisoned)
            return { LocalPipeError::transport_poisoned, 0 };
        if (states_[slot] != SlotState::connected)
            return { LocalPipeError::not_connected, 0 };
    }
    if (frame.size == 0 || frame.size > protocol_max_frame_size)
        return { LocalPipeError::invalid_frame, 0 };

    const PendingIoResult written = write_overlapped(reinterpret_cast<HANDLE>(handles_[slot]), frame.bytes.data(),
                                                      frame.size, timeout_ms);
    if (written.timed_out)
    {
        const LocalPipeResult retirement = mark_retiring(slot);
        if (!retirement.ok())
            return retirement;
        return { LocalPipeError::timeout, WAIT_TIMEOUT };
    }
    if (written.error != ERROR_SUCCESS || written.bytes != frame.size)
    {
        const DWORD error = written.error != ERROR_SUCCESS ? written.error : ERROR_WRITE_FAULT;
        const LocalPipeResult retirement = mark_retiring(slot);
        if (!retirement.ok())
            return retirement;
        return { LocalPipeError::write_failure, error };
    }
    return {};
#endif
}

LocalPipeResult OwnerPipeServer::disconnect(std::size_t slot) noexcept
{
#if !defined(_WIN32)
    (void)slot;
    return { LocalPipeError::platform_unsupported, 0 };
#else
    return mark_retiring(slot);
#endif
}

LocalPipeResult OwnerPipeServer::retire(std::size_t slot) noexcept
{
#if !defined(_WIN32)
    (void)slot;
    return { LocalPipeError::platform_unsupported, 0 };
#else
    if (slot >= owner_pipe_max_connections || handles_[slot] == 0)
        return { LocalPipeError::invalid_slot, 0 };

    std::lock_guard lock(state_mutex_);
    if (states_[slot] == SlotState::poisoned)
        return { LocalPipeError::transport_poisoned, 0 };
    if (states_[slot] != SlotState::retiring)
        return { LocalPipeError::not_retiring, 0 };
    sessions_[slot] = {};
    states_[slot] = SlotState::listening;
    return {};
#endif
}

OwnerPipeClient::~OwnerPipeClient()
{
    reset();
}

OwnerPipeClient::OwnerPipeClient(OwnerPipeClient&& other) noexcept : handle_(std::exchange(other.handle_, 0))
{
}

OwnerPipeClient& OwnerPipeClient::operator=(OwnerPipeClient&& other) noexcept
{
    if (this != &other)
    {
        reset();
        handle_ = std::exchange(other.handle_, 0);
    }
    return *this;
}

void OwnerPipeClient::reset() noexcept
{
#if defined(_WIN32)
    if (handle_ != 0)
    {
        CancelIoEx(reinterpret_cast<HANDLE>(handle_), nullptr);
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
    }
#endif
    handle_ = 0;
}

LocalPipeResult OwnerPipeClient::read_frame(ProtocolFrame& output, std::uint32_t timeout_ms) noexcept
{
#if !defined(_WIN32)
    (void)output;
    (void)timeout_ms;
    return { LocalPipeError::platform_unsupported, 0 };
#else
    if (handle_ == 0)
        return { LocalPipeError::not_connected, 0 };

    ProtocolFrame candidate {};
    const PendingIoResult read = read_overlapped(reinterpret_cast<HANDLE>(handle_), candidate.bytes.data(),
                                                  static_cast<DWORD>(candidate.bytes.size()), timeout_ms);
    if (read.timed_out)
    {
        reset();
        return { LocalPipeError::timeout, WAIT_TIMEOUT };
    }
    if (read.error != ERROR_SUCCESS)
    {
        const DWORD error = read.error;
        reset();
        if (error == ERROR_MORE_DATA)
            return { LocalPipeError::message_too_large, error };
        return { disconnected_error(error), error };
    }
    if (read.bytes == 0)
    {
        reset();
        return { LocalPipeError::empty_message, 0 };
    }

    candidate.size = static_cast<std::uint16_t>(read.bytes);
    output = candidate;
    return {};
#endif
}

LocalPipeResult OwnerPipeClient::write_frame(const ProtocolFrame& frame, std::uint32_t timeout_ms) noexcept
{
#if !defined(_WIN32)
    (void)frame;
    (void)timeout_ms;
    return { LocalPipeError::platform_unsupported, 0 };
#else
    if (handle_ == 0)
        return { LocalPipeError::not_connected, 0 };
    if (frame.size == 0 || frame.size > protocol_max_frame_size)
        return { LocalPipeError::invalid_frame, 0 };

    const PendingIoResult written = write_overlapped(reinterpret_cast<HANDLE>(handle_), frame.bytes.data(), frame.size,
                                                      timeout_ms);
    if (written.timed_out)
    {
        reset();
        return { LocalPipeError::timeout, WAIT_TIMEOUT };
    }
    if (written.error != ERROR_SUCCESS || written.bytes != frame.size)
    {
        const DWORD error = written.error != ERROR_SUCCESS ? written.error : ERROR_WRITE_FAULT;
        reset();
        return { LocalPipeError::write_failure, error };
    }
    return {};
#endif
}

OwnerPipeServerCreateResult create_owner_pipe_server() noexcept
{
#if !defined(_WIN32)
    return { { LocalPipeError::platform_unsupported, 0 }, {} };
#else
    const NameResult identity = owner_pipe_name();
    if (!identity.status.ok())
        return { identity.status, {} };

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    try
    {
        const std::wstring sddl = L"D:P(A;;GA;;;" + identity.sid + L")";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) ==
            0)
            return { { LocalPipeError::security_failure, GetLastError() }, {} };
    }
    catch (...)
    {
        return { { LocalPipeError::security_failure, ERROR_NOT_ENOUGH_MEMORY }, {} };
    }

    SECURITY_ATTRIBUTES security {};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    security.bInheritHandle = FALSE;

    std::array<std::uintptr_t, owner_pipe_max_connections> handles {};
    for (std::size_t slot = 0; slot != owner_pipe_max_connections; ++slot)
    {
        const DWORD access = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                             (slot == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        HANDLE handle = CreateNamedPipeW(identity.name.c_str(), access,
                                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                         static_cast<DWORD>(owner_pipe_max_connections),
                                         static_cast<DWORD>(protocol_max_frame_size),
                                         static_cast<DWORD>(protocol_max_frame_size), 0, &security);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            for (const std::uintptr_t created : handles)
            {
                if (created != 0)
                    CloseHandle(reinterpret_cast<HANDLE>(created));
            }
            LocalFree(descriptor);
            return { { LocalPipeError::create_failure, error }, {} };
        }
        handles[slot] = reinterpret_cast<std::uintptr_t>(handle);
    }

    LocalFree(descriptor);
    return { {}, OwnerPipeServer(handles) };
#endif
}

OwnerPipeClientConnectResult connect_owner_pipe_client() noexcept
{
#if !defined(_WIN32)
    return { { LocalPipeError::platform_unsupported, 0 }, {} };
#else
    const NameResult identity = owner_pipe_name();
    if (!identity.status.ok())
        return { identity.status, {} };

    HANDLE handle = CreateFileW(identity.name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY)
            return { { LocalPipeError::busy, error }, {} };
        if (error == ERROR_FILE_NOT_FOUND)
            return { { LocalPipeError::not_available, error }, {} };
        return { { LocalPipeError::connect_failure, error }, {} };
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (SetNamedPipeHandleState(handle, &mode, nullptr, nullptr) == 0)
    {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        return { { LocalPipeError::connect_failure, error }, {} };
    }
    return { {}, OwnerPipeClient(reinterpret_cast<std::uintptr_t>(handle)) };
#endif
}
} // namespace qiven::host
