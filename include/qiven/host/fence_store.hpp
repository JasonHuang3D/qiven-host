#pragma once
#include "qiven/host/authority.hpp"
#include <filesystem>
namespace qiven::host {
struct DurableAuthority final { PersistentGeneration generation; FencingEpoch epoch; AuthorityPhase phase; };
enum class StoreError { none, not_initialized, corrupt, io_failure, non_monotonic };
struct LoadResult { StoreError error{}; DurableAuthority value{}; bool ok() const noexcept{return error==StoreError::none;} };
class FenceStore final {
public:
 explicit FenceStore(std::filesystem::path root):root_(std::move(root)){}
 StoreError initialize(DurableAuthority value) noexcept;
 LoadResult load() const noexcept;
 StoreError store(DurableAuthority value) noexcept;
private: std::filesystem::path root_;
};
}
