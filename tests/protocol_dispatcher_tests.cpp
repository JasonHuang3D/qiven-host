#include "qiven/host/protocol_dispatcher.hpp"

#include <Windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>

using namespace qiven::host;

namespace
{
constexpr std::uint32_t accept_timeout_ms = 500;
constexpr std::uint32_t io_timeout_ms = 1000;

[[noreturn]] void fail(const char* reason)
{
    std::fprintf(stderr, "[dispatcher] FAIL: %s\n", reason);
    std::fflush(stderr);
    ExitProcess(3);
}

void require(bool value, const char* reason)
{
    if (!value)
        fail(reason);
}

template <class T>
T id(std::uint8_t value)
{
    T result {};
    result.bytes[0] = value;
    return result;
}

std::filesystem::path unique_root(const wchar_t* suffix)
{
    return std::filesystem::temp_directory_path() /
           (L"qiven-host-dispatcher-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + suffix);
}

void remove_root(const std::filesystem::path& root)
{
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "remove root");
}

ProtocolFrameView inspect(const ProtocolFrame& frame)
{
    ProtocolFrameView view {};
    require(inspect_protocol_frame(frame.view(), view).ok(), "inspect response frame");
    return view;
}

OwnerPipeClientConnectResult connect_when_listener_available(std::uint32_t timeout_ms)
{
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;)
    {
        auto connected = connect_owner_pipe_client();
        if (connected.ok())
            return connected;
        require(connected.status.error == LocalPipeError::busy, "unexpected client connect failure");
        if (GetTickCount64() >= deadline)
            fail("client remained busy past deadline");
        Sleep(1);
    }
}

struct Connection final
{
    std::size_t slot {};
    BrokerSessionId session {};
    OwnerPipeClient client {};
};

Connection connect_one(OwnerPipeServer& server)
{
    std::array<LocalPipeAcceptResult, owner_pipe_max_connections> accepted {};
    std::array<std::thread, owner_pipe_max_connections> threads;
    for (std::size_t slot = 0; slot != owner_pipe_max_connections; ++slot)
        threads[slot] = std::thread([&server, &accepted, slot] { accepted[slot] = server.accept(slot, accept_timeout_ms); });

    auto connected = connect_when_listener_available(io_timeout_ms);
    for (std::thread& thread : threads)
        thread.join();

    std::size_t selected = owner_pipe_max_connections;
    for (std::size_t slot = 0; slot != owner_pipe_max_connections; ++slot)
    {
        if (!accepted[slot].ok())
            continue;
        require(selected == owner_pipe_max_connections, "more than one slot accepted one client");
        selected = slot;
    }
    require(selected != owner_pipe_max_connections, "no slot accepted client");

    BrokerSessionId observed {};
    require(server.session(selected, observed).ok(), "read accepted session");
    require(observed == accepted[selected].session, "accepted session mismatch");
    return { selected, observed, std::move(connected.client) };
}

ProtocolFrame exchange(Connection& connection, OwnerPipeServer& server, ProtocolDispatcher& dispatcher,
                       const ProtocolFrame& request)
{
    require(connection.client.write_frame(request, io_timeout_ms).ok(), "client write request");

    ProtocolFrame received_request {};
    require(server.read_frame(connection.slot, received_request, io_timeout_ms).ok(), "server read request");

    ProtocolFrame response {};
    require(dispatcher.dispatch(connection.session, received_request, response).ok(), "dispatch request");
    require(server.write_frame(connection.slot, response, io_timeout_ms).ok(), "server write response");

    ProtocolFrame received_response {};
    require(connection.client.read_frame(received_response, io_timeout_ms).ok(), "client read response");
    return received_response;
}

AcquireResponseMessage acquire(Connection& connection, OwnerPipeServer& server, ProtocolDispatcher& dispatcher,
                               ExecutionId execution)
{
    AcquireRequestMessage request {};
    request.execution = execution;
    constexpr char scope[] = "test";
    constexpr char intent[] = "noop";
    request.scope_size = 4;
    request.intent_size = 4;
    std::memcpy(request.scope.data(), scope, request.scope_size);
    std::memcpy(request.intent.data(), intent, request.intent_size);

    ProtocolFrame encoded {};
    require(encode_protocol_message(request, encoded).ok(), "encode acquire");
    const ProtocolFrame response_frame = exchange(connection, server, dispatcher, encoded);
    AcquireResponseMessage response {};
    require(decode_protocol_message(inspect(response_frame), response).ok(), "decode acquire response");
    return response;
}

ExecuteTestOperationResponseMessage execute_no_op(Connection& connection, OwnerPipeServer& server,
                                                  ProtocolDispatcher& dispatcher, ExecutionId execution, LeaseId lease,
                                                  FencingEpoch epoch, RequestSequence sequence,
                                                  OperationId operation)
{
    ExecuteTestOperationRequestMessage request {};
    request.execution = execution;
    request.lease = lease;
    request.operation = operation;
    request.epoch = epoch;
    request.sequence = sequence;
    request.operation_code = TestOperationCode::no_op;

    ProtocolFrame encoded {};
    require(encode_protocol_message(request, encoded).ok(), "encode no-op");
    const ProtocolFrame response_frame = exchange(connection, server, dispatcher, encoded);
    ExecuteTestOperationResponseMessage response {};
    require(decode_protocol_message(inspect(response_frame), response).ok(), "decode no-op response");
    return response;
}

ReleaseResponseMessage release(Connection& connection, OwnerPipeServer& server, ProtocolDispatcher& dispatcher,
                               ExecutionId execution, LeaseId lease, FencingEpoch epoch)
{
    ReleaseRequestMessage request { execution, lease, epoch };
    ProtocolFrame encoded {};
    require(encode_protocol_message(request, encoded).ok(), "encode release");
    const ProtocolFrame response_frame = exchange(connection, server, dispatcher, encoded);
    ReleaseResponseMessage response {};
    require(decode_protocol_message(inspect(response_frame), response).ok(), "decode release response");
    return response;
}

QueryStatusResponseMessage query(Connection& connection, OwnerPipeServer& server, ProtocolDispatcher& dispatcher)
{
    QueryStatusRequestMessage request {};
    ProtocolFrame encoded {};
    require(encode_protocol_message(request, encoded).ok(), "encode query");
    const ProtocolFrame response_frame = exchange(connection, server, dispatcher, encoded);
    QueryStatusResponseMessage response {};
    require(decode_protocol_message(inspect(response_frame), response).ok(), "decode query response");
    return response;
}

void normal_flow(const std::filesystem::path& root)
{
    AuthorityBroker broker(root);
    require(broker.start().ok(), "start normal broker");
    auto created = create_owner_pipe_server();
    require(created.ok(), "create normal pipe server");
    OwnerPipeServer server = std::move(created.server);
    ProtocolDispatcher dispatcher(broker);
    Connection connection = connect_one(server);

    const ExecutionId execution = id<ExecutionId>(1);
    const AcquireResponseMessage acquired = acquire(connection, server, dispatcher, execution);
    require(acquired.result == ProtocolResultCode::ok, "acquire result");
    require(acquired.execution == execution, "acquire execution");
    require(acquired.generation.value == 1 && acquired.epoch.value == 1, "acquire fence");

    const QueryStatusResponseMessage leased = query(connection, server, dispatcher);
    require(leased.result == ProtocolResultCode::ok && leased.phase == ProtocolAuthorityPhase::leased,
            "query leased phase");
    require(leased.next_sequence.value == 1, "query initial sequence");

    const OperationId first_operation = id<OperationId>(2);
    const auto first = execute_no_op(connection, server, dispatcher, execution, acquired.lease, acquired.epoch, { 1 },
                                     first_operation);
    require(first.result == ProtocolResultCode::ok && !first.replayed, "first no-op");

    const auto replay = execute_no_op(connection, server, dispatcher, execution, acquired.lease, acquired.epoch, { 1 },
                                      first_operation);
    require(replay.result == ProtocolResultCode::ok && replay.replayed, "exact replay");

    const auto conflict = execute_no_op(connection, server, dispatcher, execution, acquired.lease, acquired.epoch, { 1 },
                                        id<OperationId>(3));
    require(conflict.result == ProtocolResultCode::replay_conflict && !conflict.replayed, "replay conflict");

    const auto gap = execute_no_op(connection, server, dispatcher, execution, acquired.lease, acquired.epoch, { 3 },
                                   id<OperationId>(4));
    require(gap.result == ProtocolResultCode::invalid_sequence, "sequence gap");

    const auto second = execute_no_op(connection, server, dispatcher, execution, acquired.lease, acquired.epoch, { 2 },
                                      id<OperationId>(5));
    require(second.result == ProtocolResultCode::ok && !second.replayed, "second no-op");

    const ReleaseResponseMessage released = release(connection, server, dispatcher, execution, acquired.lease, acquired.epoch);
    require(released.result == ProtocolResultCode::ok, "release result");

    const QueryStatusResponseMessage ready = query(connection, server, dispatcher);
    require(ready.result == ProtocolResultCode::ok && ready.phase == ProtocolAuthorityPhase::ready,
            "query ready phase");
    require(ready.next_sequence.value == 1, "release resets sequence");

    require(dispatcher.close_connection(server, connection.slot).ok(), "close released connection");
    connection.client.reset();
    require(broker.status().phase == AuthorityPhase::ready, "normal flow remains ready");
}

void disconnect_flow(const std::filesystem::path& root)
{
    AuthorityBroker broker(root);
    require(broker.start().ok(), "start disconnect broker");
    auto created = create_owner_pipe_server();
    require(created.ok(), "create disconnect pipe server");
    OwnerPipeServer server = std::move(created.server);
    ProtocolDispatcher dispatcher(broker);
    Connection connection = connect_one(server);

    const ExecutionId execution = id<ExecutionId>(11);
    const AcquireResponseMessage acquired = acquire(connection, server, dispatcher, execution);
    require(acquired.result == ProtocolResultCode::ok, "disconnect acquire");

    connection.client.reset();
    require(dispatcher.close_connection(server, connection.slot).ok(), "authority-aware disconnect close");
    require(broker.status().phase == AuthorityPhase::reconciling, "disconnect enters reconciling");

    const LocalPipeAcceptResult timeout = server.accept(connection.slot, 50);
    require(timeout.status.error == LocalPipeError::timeout, "retired slot returned to listening");
}

void competitor_flow(const std::filesystem::path& root)
{
    AuthorityBroker broker(root);
    require(broker.start().ok(), "start competitor broker");
    auto created = create_owner_pipe_server();
    require(created.ok(), "create competitor pipe server");
    OwnerPipeServer server = std::move(created.server);
    ProtocolDispatcher dispatcher(broker);

    Connection first = connect_one(server);
    Connection second = connect_one(server);
    require(first.session != second.session, "transport sessions must differ");

    const ExecutionId execution = id<ExecutionId>(21);
    const AcquireResponseMessage first_acquire = acquire(first, server, dispatcher, execution);
    require(first_acquire.result == ProtocolResultCode::ok, "first competitor acquire");

    const AcquireResponseMessage second_acquire = acquire(second, server, dispatcher, execution);
    require(second_acquire.result == ProtocolResultCode::concurrent_execution, "second competitor rejected");
    require(broker.status().phase == AuthorityPhase::quarantined, "competing transport session quarantines");

    second.client.reset();
    require(dispatcher.close_connection(server, second.slot).ok(), "close rejected competitor session");

    first.client.reset();
    require(dispatcher.close_connection(server, first.slot).ok(), "close quarantined lease session");
    require(broker.status().phase == AuthorityPhase::quarantined, "quarantine remains durable authority state");
}
} // namespace

int main()
{
    const auto normal_root = unique_root(L"normal");
    remove_root(normal_root);
    normal_flow(normal_root);
    remove_root(normal_root);

    const auto disconnect_root = unique_root(L"disconnect");
    remove_root(disconnect_root);
    disconnect_flow(disconnect_root);
    remove_root(disconnect_root);

    const auto competitor_root = unique_root(L"competitor");
    remove_root(competitor_root);
    competitor_flow(competitor_root);
    remove_root(competitor_root);

    return 0;
}
