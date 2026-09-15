#include "qiven/host/fence_store.hpp"

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
        path_ = base / ("qiven-host-fence-store-" + std::to_string(GetCurrentProcessId()) + "-" +
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

void truncate_file(const std::filesystem::path& path, std::uintmax_t size)
{
    std::filesystem::resize_file(path, size);
}
} // namespace

int main()
{
    {
        TempRoot temp;
        FenceStore store(temp.path());
        require(store.load().status.error == FenceStoreError::not_initialized);

        const DurableAuthority initial { { 1 }, { 7 }, AuthorityPhase::ready };
        require(store.initialize(initial).ok());
        const auto loaded = store.load();
        require(loaded.ok());
        require(loaded.value == initial);
        require(store.initialize(initial).error == FenceStoreError::already_initialized);

        const DurableAuthority leased_same_authority { { 1 }, { 7 }, AuthorityPhase::leased };
        require(store.store(leased_same_authority).ok());
        require(store.load().value == leased_same_authority);
        require(store.store({ { 0 }, { 7 }, AuthorityPhase::ready }).error == FenceStoreError::non_monotonic);
        require(store.store({ { 2 }, { 6 }, AuthorityPhase::ready }).error == FenceStoreError::non_monotonic);
        require(store.store({ { 1 }, { 8 }, AuthorityPhase::ready }).error == FenceStoreError::non_monotonic);

        overwrite_byte(temp.path() / "fence.b", 32);
        const auto recovered = store.load();
        require(recovered.ok());
        require(recovered.value == initial);

        const DurableAuthority recovered_generation { { 2 }, { 8 }, AuthorityPhase::reconciling };
        require(store.store(recovered_generation).ok());
        require(store.load().value == recovered_generation);

        truncate_file(temp.path() / "fence.b", 19);
        const auto truncated = store.load();
        require(truncated.ok());
        require(truncated.value == initial);

        overwrite_byte(temp.path() / "fence.a", 24);
        require(store.load().status.error == FenceStoreError::corrupt);
        require(store.store({ { 3 }, { 9 }, AuthorityPhase::quarantined }).error == FenceStoreError::corrupt);
        require(store.initialize(initial).error == FenceStoreError::corrupt);
    }

    {
        TempRoot temp;
        std::filesystem::create_directories(temp.path());
        FenceStore interrupted_bootstrap(temp.path());
        require(interrupted_bootstrap.load().status.error == FenceStoreError::corrupt);
        require(interrupted_bootstrap.initialize({ { 1 }, { 1 }, AuthorityPhase::ready }).error == FenceStoreError::corrupt);
    }

    {
        TempRoot temp;
        FenceStore erased(temp.path());
        require(erased.initialize({ { 4 }, { 9 }, AuthorityPhase::ready }).ok());
        std::filesystem::remove(temp.path() / "fence.a");
        std::filesystem::remove(temp.path() / "fence.b");
        require(erased.load().status.error == FenceStoreError::corrupt);
        require(erased.initialize({ { 1 }, { 1 }, AuthorityPhase::ready }).error == FenceStoreError::corrupt);
    }

    {
        TempRoot temp;
        const auto occupied = temp.path();
        {
            std::ofstream file(occupied, std::ios::binary);
            file << 'x';
        }
        FenceStore blocked(occupied);
        const auto result = blocked.initialize({ { 1 }, { 1 }, AuthorityPhase::ready });
        require(result.error == FenceStoreError::io_failure);
    }

    return 0;
}
