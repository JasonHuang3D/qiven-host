#include "qiven/host/fence_store.hpp"
#include <array>
#include <fstream>
namespace qiven::host { namespace {
constexpr std::uint64_t magic=0x51485646; constexpr std::size_t size=48;
void put(std::array<std::uint8_t,size>&b,std::size_t o,std::uint64_t v){for(unsigned i=0;i<8;++i)b[o+i]=static_cast<std::uint8_t>(v>>(i*8));}
std::uint64_t get(const std::array<std::uint8_t,size>&b,std::size_t o){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t{b[o+i]}<<(i*8);return v;}
std::uint32_t crc(const std::uint8_t*p,std::size_t n){std::uint32_t c=0xffffffffu;for(std::size_t i=0;i<n;++i){c^=p[i];for(int k=0;k<8;++k)c=(c>>1)^((c&1)?0xedb88320u:0);}return ~c;}
bool read(const std::filesystem::path&p,DurableAuthority&v){std::array<std::uint8_t,size>b{};std::ifstream f(p,std::ios::binary);if(!f.read(reinterpret_cast<char*>(b.data()),size)||f.peek()!=EOF)return false;if(get(b,0)!=magic||get(b,8)!=1||get(b,40)!=crc(b.data(),40))return false;auto ph=get(b,32);if(ph>3)return false;v={{get(b,16)},{get(b,24)},static_cast<AuthorityPhase>(ph)};return true;}
bool write(const std::filesystem::path&p,DurableAuthority v){std::array<std::uint8_t,size>b{};put(b,0,magic);put(b,8,1);put(b,16,v.generation.value);put(b,24,v.epoch.value);put(b,32,static_cast<std::uint64_t>(v.phase));put(b,40,crc(b.data(),40));std::ofstream f(p,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(b.data()),size);f.flush();return f.good();}
}
LoadResult FenceStore::load()const noexcept{try{DurableAuthority a{},b{};bool av=read(root_/"fence.a",a),bv=read(root_/"fence.b",b);if(!av&&!bv)return {std::filesystem::exists(root_/"initialized")?StoreError::corrupt:StoreError::not_initialized,{}};return {StoreError::none,(!bv||(av&&a.generation.value>=b.generation.value))?a:b};}catch(...){return {StoreError::io_failure,{}};}}
StoreError FenceStore::initialize(DurableAuthority v)noexcept{try{std::filesystem::create_directories(root_);if(std::filesystem::exists(root_/"initialized"))return StoreError::non_monotonic;if(!write(root_/"fence.a",v))return StoreError::io_failure;std::ofstream(root_/"initialized",std::ios::binary).put('1');return StoreError::none;}catch(...){return StoreError::io_failure;}}
StoreError FenceStore::store(DurableAuthority v)noexcept{auto old=load();if(!old.ok())return old.error;if(v.generation.value<=old.value.generation.value||v.epoch.value<old.value.epoch.value)return StoreError::non_monotonic;auto target=(old.value.generation.value%2==0)?root_/"fence.a":root_/"fence.b";return write(target,v)?StoreError::none:StoreError::io_failure;}
}
