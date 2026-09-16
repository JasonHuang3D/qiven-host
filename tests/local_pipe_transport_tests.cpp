#include "qiven/host/local_pipe_transport.hpp"

#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace qiven::host;

namespace qiven::host
{
class TestLocalPipeAccess final
{
public:
    static bool invalidate_server_handle(OwnerPipeServer& server, std::size_t slot) noexcept
    {
        if (slot >= owner_pipe_max_connections || server.handles_[slot] == 0)
            return false;
        return CloseHandle(reinterpret_cast<HANDLE>(server.handles_[slot])) != 0;
    }
};
} // namespace qiven::host

namespace
{
constexpr std::uint32_t test_timeout_ms = 1000;
constexpr std::uint32_t short_timeout_ms = 100;

void require(bool value)
{
    if (!value)
        std::abort();
}

ProtocolFrameView inspect(const ProtocolFrame& frame)
{
    ProtocolFrameView view {};
    require(inspect_protocol_frame(frame.view(), view).ok());
    return view;
}

bool nonzero(BrokerSessionId value)
{
    for (std::uint8_t byte : value.bytes)
    {
        if (byte != 0)
            return true;
    }
    return false;
}

std::wstring current_user_sid_text()
{
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0);

    DWORD bytes = 0;
    SetLastError(ERROR_SUCCESS);
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes != 0);

    void* buffer = HeapAlloc(GetProcessHeap(), 0, bytes);
    require(buffer != nullptr);
    require(GetTokenInformation(token, TokenUser, buffer, bytes, &bytes) != 0);
    require(CloseHandle(token) != 0);

    const auto* user = static_cast<const TOKEN_USER*>(buffer);
    LPWSTR text = nullptr;
    require(ConvertSidToStringSidW(user->User.Sid, &text) != 0);
    const std::wstring result = text;
    LocalFree(text);
    require(HeapFree(GetProcessHeap(), 0, buffer) != 0);
    return result;
}

std::wstring pipe_name(std::wstring_view sid)
{
    return L"\\\\.\\pipe\\QivenHost.Broker." + std::wstring(sid);
}

HANDLE connect_raw(const std::wstring& name, DWORD extra_access = 0)
{
    HANDLE handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE | extra_access, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    require(handle != INVALID_HANDLE_VALUE);
    DWORD mode = PIPE_READMODE_MESSAGE;
    require(SetNamedPipeHandleState(handle, &mode, nullptr, nullptr) != 0);
    return handle;
}

void verify_owner_only_dacl(HANDLE handle, std::wstring_view expected_owner)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                         nullptr, &descriptor);
    require(status == ERROR_SUCCESS);
    require(descriptor != nullptr && dacl != nullptr);

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    require(GetSecurityDescriptorControl(descriptor, &control, &revision) != 0);
    require((control & SE_DACL_PROTECTED) != 0);

    ACL_SIZE_INFORMATION info {};
    require(GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation) != 0);
    require(info.AceCount == 1);

    void* raw_ace = nullptr;
    require(GetAce(dacl, 0, &raw_ace) != 0);
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    require(header->AceType == ACCESS_ALLOWED_ACE_TYPE);
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);

    LPWSTR ace_sid_text = nullptr;
    require(ConvertSidToStringSidW(const_cast<void*>(static_cast<const void*>(&ace->SidStart)), &ace_sid_text) != 0);
    require(expected_owner == std::wstring_view(ace_sid_text));
    LocalFree(ace_sid_text);

    require((ace->Mask & GENERIC_ALL) != 0 || (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS);
    LocalFree(descriptor);
}

struct RawWrite final
{
    OVERLAPPED overlap {};
    DWORD immediate_bytes {};
    bool immediate {};
};

RawWrite start_raw_write(HANDLE handle, const void* data, DWORD size)
{
    RawWrite result;
    result.overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(result.overlap.hEvent != nullptr);
    result.immediate = WriteFile(handle, data, size, &result.immediate_bytes, &result.overlap) != 0;
    if (!result.immediate)
        require(GetLastError() == ERROR_IO_PENDING);
    return result;
}

void finish_raw_write(HANDLE handle, RawWrite& write)
{
    if (!write.immediate)
    {
        DWORD wait = WaitForSingleObject(write.overlap.hEvent, test_timeout_ms);
        if (wait == WAIT_TIMEOUT)
        {
            CancelIoEx(handle, &write.overlap);
            wait = WaitForSingleObject(write.overlap.hEvent, test_timeout_ms);
        }
        require(wait == WAIT_OBJECT_0);
        DWORD bytes = 0;
        GetOverlappedResult(handle, &write.overlap, &bytes, FALSE);
    }
    require(CloseHandle(write.overlap.hEvent) != 0);
    write.overlap.hEvent = nullptr;
}
} // namespace

int main()
{
    static_assert(owner_pipe_max_connections == 4);
    static_assert(owner_pipe_default_timeout_ms == 5000);

    const std::wstring owner_sid = current_user_sid_text();
    const std::wstring name = pipe_name(owner_sid);

    auto created = create_owner_pipe_server();
    require(created.ok());
    OwnerPipeServer server = std::move(created.server);

    auto duplicate = create_owner_pipe_server();
    require(!duplicate.ok());
    require(duplicate.status.error == LocalPipeError::create_failure);

    const auto accept_timeout = server.accept(0, short_timeout_ms);
    require(accept_timeout.status.error == LocalPipeError::timeout);

    std::array<LocalPipeAcceptResult, owner_pipe_max_connections> accepted {};
    std::array<std::thread, owner_pipe_max_connections> accept_threads;
    for (std::size_t slot = 0; slot != owner_pipe_max_connections; ++slot)
        accept_threads[slot] = std::thread([&server, &accepted, slot] { accepted[slot] = server.accept(slot, test_timeout_ms); });

    HANDLE raw = connect_raw(name, READ_CONTROL);
    verify_owner_only_dacl(raw, owner_sid);

    std::array<OwnerPipeClient, 3> clients;
    for (OwnerPipeClient& client : clients)
    {
        auto connected = connect_owner_pipe_client();
        require(connected.ok());
        client = std::move(connected.client);
    }

    for (std::thread& thread : accept_threads)
        thread.join();

    for (std::size_t slot = 0; slot != owner_pipe_max_connections; ++slot)
    {
        require(accepted[slot].ok());
        require(nonzero(accepted[slot].session));
        BrokerSessionId observed {};
        require(server.session(slot, observed).ok());
        require(observed == accepted[slot].session);
        for (std::size_t prior = 0; prior != slot; ++prior)
            require(accepted[slot].session != accepted[prior].session);
    }

    auto saturated = connect_owner_pipe_client();
    require(!saturated.ok());
    require(saturated.status.error == LocalPipeError::busy);

    QueryStatusRequestMessage query {};
    ProtocolFrame query_frame {};
    require(encode_protocol_message(query, query_frame).ok());
    for (OwnerPipeClient& client : clients)
        require(client.write_frame(query_frame, test_timeout_ms).ok());

    std::array<std::byte, protocol_max_frame_size + 1> oversized {};
    RawWrite raw_write = start_raw_write(raw, oversized.data(), static_cast<DWORD>(oversized.size()));

    std::size_t oversized_slot = owner_pipe_max_connections;
    std::array<bool, owner_pipe_max_connections> readable {};
    for (std::size_t slot = 0; slot != owner_pipe_max_connections; ++slot)
    {
        ProtocolFrame frame {};
        frame.size = 13;
        const LocalPipeResult read = server.read_frame(slot, frame, test_timeout_ms);
        if (read.error == LocalPipeError::message_too_large)
        {
            require(oversized_slot == owner_pipe_max_connections);
            require(frame.size == 13);
            oversized_slot = slot;
            continue;
        }
        require(read.ok());
        ProtocolFrameView view {};
        require(inspect_protocol_frame(frame.view(), view).ok());
        require(view.kind == ProtocolMessageKind::query_status_request);
        readable[slot] = true;
    }
    finish_raw_write(raw, raw_write);
    require(oversized_slot < owner_pipe_max_connections);
    CloseHandle(raw);

    QueryStatusResponseMessage status {};
    status.result = ProtocolResultCode::ok;
    status.started = true;
    status.phase = ProtocolAuthorityPhase::ready;
    status.generation = { 1 };
    status.epoch = { 1 };
    status.next_sequence = { 1 };
    ProtocolFrame response {};
    require(encode_protocol_message(status, response).ok());

    for (std::size_t slot = 0; slot != owner_pipe_max_connections; ++slot)
    {
        if (readable[slot])
            require(server.write_frame(slot, response, test_timeout_ms).ok());
    }
    for (OwnerPipeClient& client : clients)
    {
        ProtocolFrame received {};
        require(client.read_frame(received, test_timeout_ms).ok());
        QueryStatusResponseMessage decoded {};
        require(decode_protocol_message(inspect(received), decoded).ok());
        require(decoded.result == ProtocolResultCode::ok && decoded.started);
    }

    const BrokerSessionId oversized_session = accepted[oversized_slot].session;
    BrokerSessionId preserved {};
    require(server.session(oversized_slot, preserved).ok());
    require(preserved == oversized_session);
    require(server.accept(oversized_slot, short_timeout_ms).status.error == LocalPipeError::retirement_required);
    require(server.retire(oversized_slot).ok());
    BrokerSessionId sentinel {};
    sentinel.bytes[0] = 99;
    require(server.session(oversized_slot, sentinel).error == LocalPipeError::not_connected);
    require(sentinel.bytes[0] == 99);

    LocalPipeAcceptResult replacement_accept {};
    std::thread replacement_thread([&] { replacement_accept = server.accept(oversized_slot, test_timeout_ms); });
    auto replacement_connected = connect_owner_pipe_client();
    require(replacement_connected.ok());
    OwnerPipeClient replacement = std::move(replacement_connected.client);
    replacement_thread.join();
    require(replacement_accept.ok());
    require(nonzero(replacement_accept.session));
    require(replacement_accept.session != oversized_session);

    replacement.reset();
    ProtocolFrame ignored {};
    const LocalPipeResult disconnected = server.read_frame(oversized_slot, ignored, test_timeout_ms);
    require(disconnected.error == LocalPipeError::disconnected);
    BrokerSessionId disconnect_session {};
    require(server.session(oversized_slot, disconnect_session).ok());
    require(disconnect_session == replacement_accept.session);
    require(server.accept(oversized_slot, short_timeout_ms).status.error == LocalPipeError::retirement_required);
    require(server.retire(oversized_slot).ok());

    LocalPipeAcceptResult silent_accept {};
    std::thread silent_thread([&] { silent_accept = server.accept(oversized_slot, test_timeout_ms); });
    auto silent_connected = connect_owner_pipe_client();
    require(silent_connected.ok());
    OwnerPipeClient silent = std::move(silent_connected.client);
    silent_thread.join();
    require(silent_accept.ok());
    ProtocolFrame silent_output {};
    silent_output.size = 31;
    const auto silent_timeout = server.read_frame(oversized_slot, silent_output, short_timeout_ms);
    require(silent_timeout.error == LocalPipeError::timeout);
    require(silent_output.size == 31);
    BrokerSessionId silent_preserved {};
    require(server.session(oversized_slot, silent_preserved).ok());
    require(silent_preserved == silent_accept.session);
    require(server.retire(oversized_slot).ok());
    silent.reset();

    ProtocolFrame invalid {};
    invalid.size = static_cast<std::uint16_t>(protocol_max_frame_size + 1);
    require(clients[0].write_frame(invalid, test_timeout_ms).error == LocalPipeError::invalid_frame);

    std::size_t poison_slot = owner_pipe_max_connections;
    for (std::size_t slot = 0; slot != owner_pipe_max_connections; ++slot)
    {
        BrokerSessionId current {};
        if (server.session(slot, current).ok())
        {
            poison_slot = slot;
            break;
        }
    }
    require(poison_slot < owner_pipe_max_connections);
    BrokerSessionId poison_session {};
    require(server.session(poison_slot, poison_session).ok());
    require(TestLocalPipeAccess::invalidate_server_handle(server, poison_slot));

    ProtocolFrame poison_output {};
    poison_output.size = 23;
    const LocalPipeResult poisoned = server.read_frame(poison_slot, poison_output, short_timeout_ms);
    require(poisoned.error == LocalPipeError::transport_poisoned);
    require(poison_output.size == 23);
    BrokerSessionId poison_preserved {};
    require(server.session(poison_slot, poison_preserved).ok());
    require(poison_preserved == poison_session);
    require(server.retire(poison_slot).error == LocalPipeError::transport_poisoned);
    require(server.accept(poison_slot, short_timeout_ms).status.error == LocalPipeError::transport_poisoned);

    return 0;
}
