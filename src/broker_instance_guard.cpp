#include "qiven/host/broker_instance_guard.hpp"

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
#include <sddl.h>
#endif

namespace qiven::host
{
namespace
{
#if defined(_WIN32)
struct SidResult final
{
    BrokerInstanceError error {};
    std::uint32_t native_error {};
    std::wstring text;
};

SidResult current_user_sid() noexcept
{
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0)
        return { BrokerInstanceError::identity_failure, GetLastError(), {} };

    DWORD bytes = 0;
    SetLastError(ERROR_SUCCESS);
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    const auto size_error = GetLastError();
    if (bytes == 0 || size_error != ERROR_INSUFFICIENT_BUFFER)
    {
        const auto close_error = CloseHandle(token) == 0 ? GetLastError() : ERROR_SUCCESS;
        return { BrokerInstanceError::identity_failure, size_error != ERROR_SUCCESS ? size_error : close_error, {} };
    }

    void* buffer = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (buffer == nullptr)
    {
        CloseHandle(token);
        return { BrokerInstanceError::system_failure, ERROR_NOT_ENOUGH_MEMORY, {} };
    }

    if (GetTokenInformation(token, TokenUser, buffer, bytes, &bytes) == 0)
    {
        const auto error = GetLastError();
        HeapFree(GetProcessHeap(), 0, buffer);
        CloseHandle(token);
        return { BrokerInstanceError::identity_failure, error, {} };
    }

    const auto* user = static_cast<const TOKEN_USER*>(buffer);
    LPWSTR sid_text = nullptr;
    if (ConvertSidToStringSidW(user->User.Sid, &sid_text) == 0)
    {
        const auto error = GetLastError();
        HeapFree(GetProcessHeap(), 0, buffer);
        CloseHandle(token);
        return { BrokerInstanceError::identity_failure, error, {} };
    }

    SidResult result;
    try
    {
        result.text = sid_text;
    }
    catch (...)
    {
        result.error = BrokerInstanceError::system_failure;
        result.native_error = ERROR_NOT_ENOUGH_MEMORY;
    }

    LocalFree(sid_text);
    HeapFree(GetProcessHeap(), 0, buffer);
    if (CloseHandle(token) == 0 && result.error == BrokerInstanceError::none)
    {
        result.error = BrokerInstanceError::system_failure;
        result.native_error = GetLastError();
    }
    return result;
}
#endif
} // namespace

BrokerInstanceGuard::~BrokerInstanceGuard()
{
    reset();
}

BrokerInstanceGuard::BrokerInstanceGuard(BrokerInstanceGuard&& other) noexcept : handle_(std::exchange(other.handle_, 0))
{
}

BrokerInstanceGuard& BrokerInstanceGuard::operator=(BrokerInstanceGuard&& other) noexcept
{
    if (this != &other)
    {
        reset();
        handle_ = std::exchange(other.handle_, 0);
    }
    return *this;
}

void BrokerInstanceGuard::reset() noexcept
{
#if defined(_WIN32)
    if (handle_ != 0)
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
#endif
    handle_ = 0;
}

BrokerInstanceAcquireResult acquire_broker_instance() noexcept
{
#if defined(_WIN32)
    const SidResult sid = current_user_sid();
    if (sid.error != BrokerInstanceError::none)
        return { sid.error, sid.native_error, {} };

    try
    {
        const std::wstring name = L"Global\\QivenHost.Broker." + sid.text;
        SetLastError(ERROR_SUCCESS);
        HANDLE handle = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle == nullptr)
            return { BrokerInstanceError::system_failure, GetLastError(), {} };

        const auto create_status = GetLastError();
        if (create_status == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(handle);
            return { BrokerInstanceError::already_running, create_status, {} };
        }
        if (create_status != ERROR_SUCCESS)
        {
            CloseHandle(handle);
            return { BrokerInstanceError::system_failure, create_status, {} };
        }

        return { BrokerInstanceError::none, 0, BrokerInstanceGuard(reinterpret_cast<std::uintptr_t>(handle)) };
    }
    catch (...)
    {
        return { BrokerInstanceError::system_failure, ERROR_NOT_ENOUGH_MEMORY, {} };
    }
#else
    return { BrokerInstanceError::platform_unsupported, 0, {} };
#endif
}
} // namespace qiven::host
