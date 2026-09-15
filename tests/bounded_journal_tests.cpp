#include "qiven/host/bounded_journal.hpp"

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace qiven::host;

namespace
{
void require(bool value)
{
    if (!value)
        std::abort();
}

class TempRoot final
{
public:
    TempRoot()
    {
        const auto base = std::filesystem::temp_directory_path();
        path_ = base / ("qiven-host-journal-" + std::to_string(GetCurrentProcessId()) + "-" +
                        std::to_string(GetTickCount64()) + "-" + std::to_string(counter_++));
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    ~TempRoot()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    static inline std::uint64_t counter_ {};
    std::filesystem::path path_;
};

JournalDigest digest(std::uint8_t seed)
{
    JournalDigest value {};
    for (std::size_t i = 0; i != value.size(); ++i)
        value[i] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(i));
    return value;
}

bool is_zero(const JournalDigest& value)
{
    for (const auto byte : value)
    {
        if (byte != 0)
            return false;
    }
    return true;
}

JournalEntry entry(std::uint64_t sequence)
{
    const auto seed = static_cast<std::uint8_t>(sequence & 0xffu);
    return { { 1 },
             { 7 },
             { sequence },
             digest(static_cast<std::uint8_t>(seed + 1)),
             digest(static_cast<std::uint8_t>(seed + 2)),
             digest(static_cast<std::uint8_t>(seed + 3)),
             digest(static_cast<std::uint8_t>(seed + 4)),
             JournalEvent::operation_completed,
             JournalOutcome::success };
}

void overwrite_byte(const std::filesystem::path& path, std::streamoff offset)
{
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    require(stream.good());
    stream.seekg(offset);
    char value = 0;
    stream.read(&value, 1);
    require(stream.good());
    value = static_cast<char>(value ^ 0x5a);
    stream.seekp(offset);
    stream.write(&value, 1);
    stream.flush();
    require(stream.good());
}
} // namespace

int main()
{
    {
        TempRoot temp;
        BoundedJournal journal(temp.path());
        require(journal.head().error == JournalError::not_initialized);

        const auto initialized = journal.initialize();
        require(initialized.ok());
        require(initialized.head.index == 0);
        require(is_zero(initialized.head.digest));
        require(journal.initialize().error == JournalError::already_initialized);

        JournalEntry invalid = entry(1);
        invalid.event = static_cast<JournalEvent>(255);
        require(journal.append(invalid).error == JournalError::invalid_value);
        require(journal.head().head.index == 0);

        const auto first = journal.append(entry(1));
        require(first.ok());
        require(first.head.index == 1);
        require(!is_zero(first.head.digest));

        const auto second = journal.append(entry(2));
        require(second.ok());
        require(second.head.index == 2);
        require(second.head.digest != first.head.digest);

        BoundedJournal reopened(temp.path());
        require(reopened.head().head == second.head);

        for (std::uint64_t index = 3; index <= 130; ++index)
        {
            const auto appended = reopened.append(entry(index));
            require(appended.ok());
            require(appended.head.index == index);
        }
        const auto rotated_head = reopened.head();
        require(rotated_head.ok());
        require(rotated_head.head.index == 130);

        const auto a_size = std::filesystem::file_size(temp.path() / "journal.a");
        const auto b_size = std::filesystem::file_size(temp.path() / "journal.b");
        require(a_size + b_size <= BoundedJournal::maximum_retained_bytes);
        require(b_size == 17'488);
    }

    {
        TempRoot temp;
        BoundedJournal journal(temp.path());
        require(journal.initialize().ok());
        require(journal.append(entry(1)).ok());
        overwrite_byte(temp.path() / "journal.a", 80 + 64);
        require(journal.head().error == JournalError::corrupt);
        require(journal.append(entry(2)).error == JournalError::corrupt);
    }

    {
        TempRoot temp;
        BoundedJournal journal(temp.path());
        require(journal.initialize().ok());
        require(journal.append(entry(1)).ok());
        const auto path = temp.path() / "journal.a";
        const auto size = std::filesystem::file_size(path);
        require(size > 0);
        std::filesystem::resize_file(path, size - 1);
        require(journal.head().error == JournalError::corrupt);
    }

    {
        TempRoot temp;
        std::filesystem::create_directories(temp.path());
        BoundedJournal interrupted_bootstrap(temp.path());
        require(interrupted_bootstrap.head().error == JournalError::corrupt);
        require(interrupted_bootstrap.initialize().error == JournalError::corrupt);
    }

    {
        TempRoot temp;
        BoundedJournal journal(temp.path());
        require(journal.initialize().ok());
        for (std::uint64_t index = 1; index <= 65; ++index)
            require(journal.append(entry(index)).ok());
        std::filesystem::remove(temp.path() / "journal.a");
        require(journal.head().error == JournalError::corrupt);
    }

    {
        TempRoot temp;
        BoundedJournal journal(temp.path());
        require(journal.initialize().ok());
        for (std::uint64_t index = 1; index <= 65; ++index)
            require(journal.append(entry(index)).ok());
        overwrite_byte(temp.path() / "journal.a", 32);
        require(journal.head().error == JournalError::corrupt);
    }

    {
        TempRoot temp;
        {
            std::ofstream file(temp.path(), std::ios::binary);
            file << 'x';
        }
        BoundedJournal blocked(temp.path());
        require(blocked.initialize().error == JournalError::io_failure);
    }

    return 0;
}
