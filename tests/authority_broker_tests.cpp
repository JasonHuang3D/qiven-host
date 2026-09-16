#include "qiven/host/authority_broker.hpp"

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace qiven::host;

namespace
{
template <class T>
T id(std::uint8_t value)
{
    T result {};
    result.bytes[0] = value;
    return result;
}

void require(bool value)
{
    if (!value)
        std::abort();
}

std::filesystem::path unique_root(const wchar_t* suffix)
{
    return std::filesystem::temp_directory_path() /
           (L"qiven-host-broker-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + suffix);
}

void remove_root(const std::filesystem::path& root)
{
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error);
}

void truncate_file(const std::filesystem::path& path)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE);
    require(CloseHandle(handle) != 0);
}
} // namespace

int main()
{
    const BrokerSessionId session = id<BrokerSessionId>(1);
    const BrokerSessionId competitor_session = id<BrokerSessionId>(9);
    const ExecutionId execution = id<ExecutionId>(2);
    const LeaseId lease_id = id<LeaseId>(3);
    const OperationId operation = id<OperationId>(4);

    const auto restart_root = unique_root(L"restart");
    remove_root(restart_root);
    {
        AuthorityBroker broker(restart_root);
        require(broker.start().ok());
        auto state = broker.status();
        require(state.started && !state.persistence_fault && state.phase == AuthorityPhase::ready);
        require(state.generation.value == 1 && state.epoch.value == 1);

        const auto acquired = broker.acquire(session, execution, lease_id);
        require(acquired.ok());
        const Request request { acquired.lease, { 1 }, operation, 10 };
        require(broker.begin_operation(session, request).ok());
    }
    {
        AuthorityBroker broker(restart_root);
        require(broker.start().ok());
        const auto state = broker.status();
        require(state.phase == AuthorityPhase::reconciling);
        require(broker.acquire(session, execution, lease_id).status.authority_error == AuthorityError::reconciling);
    }
    remove_root(restart_root);

    const auto clean_root = unique_root(L"clean");
    remove_root(clean_root);
    {
        AuthorityBroker broker(clean_root);
        require(broker.start().ok());
        const auto acquired = broker.acquire(session, execution, lease_id);
        require(acquired.ok());
        const Request request { acquired.lease, { 1 }, operation, 11 };
        require(broker.begin_operation(session, request).ok());
        require(broker.finish_operation(session, request, OperationOutcome::success).ok());
        require(broker.release(session, acquired.lease).ok());
        require(broker.status().phase == AuthorityPhase::ready);
    }
    {
        AuthorityBroker broker(clean_root);
        require(broker.start().ok());
        require(broker.status().phase == AuthorityPhase::ready);
    }
    remove_root(clean_root);

    const auto quarantine_root = unique_root(L"quarantine");
    remove_root(quarantine_root);
    {
        AuthorityBroker broker(quarantine_root);
        require(broker.start().ok());
        const auto acquired = broker.acquire(session, execution, lease_id);
        require(acquired.ok());
        require(broker.release(competitor_session, acquired.lease).authority_error == AuthorityError::concurrent_execution);
        require(broker.status().phase == AuthorityPhase::quarantined);
    }
    {
        AuthorityBroker broker(quarantine_root);
        require(broker.start().ok());
        require(broker.status().phase == AuthorityPhase::quarantined);
    }
    remove_root(quarantine_root);

    const auto partial_root = unique_root(L"partial");
    remove_root(partial_root);
    {
        FenceStore fence(partial_root / "authority");
        require(fence.initialize({ { 1 }, { 1 }, AuthorityPhase::ready }).ok());
        AuthorityBroker broker(partial_root);
        const auto started = broker.start();
        require(started.error == AuthorityBrokerError::inconsistent_persistence);
        require(broker.status().persistence_fault);
    }
    remove_root(partial_root);

    const auto fault_root = unique_root(L"fault");
    remove_root(fault_root);
    {
        AuthorityBroker broker(fault_root);
        require(broker.start().ok());
        const auto acquired = broker.acquire(session, execution, lease_id);
        require(acquired.ok());
        truncate_file(fault_root / "journal" / "journal.a");
        const Request request { acquired.lease, { 1 }, operation, 12 };
        const auto admitted = broker.begin_operation(session, request);
        require(admitted.error == AuthorityBrokerError::journal_failure);
        require(broker.status().persistence_fault);
        require(broker.release(session, acquired.lease).error == AuthorityBrokerError::journal_failure);
    }
    {
        AuthorityBroker broker(fault_root);
        require(broker.start().error == AuthorityBrokerError::journal_failure);
        require(broker.status().persistence_fault);
    }
    remove_root(fault_root);

    return 0;
}
