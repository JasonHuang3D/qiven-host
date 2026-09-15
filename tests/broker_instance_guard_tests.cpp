#include "qiven/host/broker_instance_guard.hpp"

#include <Windows.h>

#include <array>
#include <cstdlib>
#include <string>

using namespace qiven::host;

namespace
{
constexpr DWORD loser_exit = 10;
constexpr DWORD failure_exit = 20;
constexpr std::size_t contender_count = 8;

void require(bool value)
{
    if (!value)
        std::abort();
}

std::wstring unique_name(const wchar_t* suffix)
{
    return L"Local\\QivenHostTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) +
           L"." + suffix;
}

int contender(const wchar_t* start_name, const wchar_t* release_name, const wchar_t* attempted_name)
{
    HANDLE start = OpenEventW(SYNCHRONIZE, FALSE, start_name);
    HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, release_name);
    HANDLE attempted = OpenSemaphoreW(SEMAPHORE_MODIFY_STATE, FALSE, attempted_name);
    if (start == nullptr || release == nullptr || attempted == nullptr)
        return static_cast<int>(failure_exit);

    if (WaitForSingleObject(start, 10'000) != WAIT_OBJECT_0)
        return static_cast<int>(failure_exit);

    auto acquired = acquire_broker_instance();
    const bool winner = acquired.ok();
    const bool expected_loser = acquired.error == BrokerInstanceError::already_running;
    if (!winner && !expected_loser)
        return static_cast<int>(failure_exit);

    if (ReleaseSemaphore(attempted, 1, nullptr) == 0)
        return static_cast<int>(failure_exit);

    if (winner && WaitForSingleObject(release, 10'000) != WAIT_OBJECT_0)
        return static_cast<int>(failure_exit);

    CloseHandle(attempted);
    CloseHandle(release);
    CloseHandle(start);
    return winner ? 0 : static_cast<int>(loser_exit);
}

std::wstring executable_path()
{
    std::wstring path(512, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        require(length != 0);
        if (length < path.size() - 1)
        {
            path.resize(length);
            return path;
        }
        require(path.size() < 32'768);
        path.resize(path.size() * 2);
    }
}

PROCESS_INFORMATION spawn_contender(const std::wstring& exe, const std::wstring& start, const std::wstring& release,
                                    const std::wstring& attempted)
{
    std::wstring command = L"\"" + exe + L"\" --contender \"" + start + L"\" \"" + release + L"\" \"" +
                           attempted + L"\"";
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != 0);
    CloseHandle(process.hThread);
    process.hThread = nullptr;
    return process;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc == 5 && std::wstring(argv[1]) == L"--contender")
        return contender(argv[2], argv[3], argv[4]);

    {
        auto first = acquire_broker_instance();
        require(first.ok());
        require(static_cast<bool>(first.guard));
        auto second = acquire_broker_instance();
        require(second.error == BrokerInstanceError::already_running);
    }
    require(acquire_broker_instance().ok());

    const std::wstring start_name = unique_name(L"start");
    const std::wstring release_name = unique_name(L"release");
    const std::wstring attempted_name = unique_name(L"attempted");
    HANDLE start = CreateEventW(nullptr, TRUE, FALSE, start_name.c_str());
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, release_name.c_str());
    HANDLE attempted = CreateSemaphoreW(nullptr, 0, static_cast<LONG>(contender_count), attempted_name.c_str());
    require(start != nullptr && release != nullptr && attempted != nullptr);

    const auto exe = executable_path();
    std::array<PROCESS_INFORMATION, contender_count> processes {};
    for (auto& process : processes)
        process = spawn_contender(exe, start_name, release_name, attempted_name);

    require(SetEvent(start) != 0);
    for (std::size_t i = 0; i != contender_count; ++i)
        require(WaitForSingleObject(attempted, 10'000) == WAIT_OBJECT_0);

    DWORD active = static_cast<DWORD>(contender_count);
    const ULONGLONG deadline = GetTickCount64() + 5'000;
    while (active != 1 && GetTickCount64() < deadline)
    {
        active = 0;
        for (const auto& process : processes)
        {
            DWORD code = 0;
            require(GetExitCodeProcess(process.hProcess, &code) != 0);
            if (code == STILL_ACTIVE)
                ++active;
            else
                require(code == loser_exit);
        }
        if (active != 1)
            Sleep(10);
    }
    require(active == 1);

    require(SetEvent(release) != 0);
    DWORD winners = 0;
    for (auto& process : processes)
    {
        require(WaitForSingleObject(process.hProcess, 10'000) == WAIT_OBJECT_0);
        DWORD code = 0;
        require(GetExitCodeProcess(process.hProcess, &code) != 0);
        if (code == 0)
            ++winners;
        else
            require(code == loser_exit);
        CloseHandle(process.hProcess);
    }
    require(winners == 1);

    CloseHandle(attempted);
    CloseHandle(release);
    CloseHandle(start);
    return 0;
}
