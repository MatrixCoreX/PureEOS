#include <eosio/chain/reversible_recovery.hpp>
#include <eosio/chain/performance_metrics.hpp>
#include <eosio/chain/exceptions.hpp>
#include <fc/io/cfile.hpp>
#include <fc/io/raw.hpp>
#include <fc/scoped_exit.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <tuple>
#include <unistd.h>

namespace eosio::chain {
namespace {
constexpr uint64_t recovery_magic = 0x3152564345524550; // PERECVR1, format v1
constexpr size_t recovery_prefix_size = sizeof(recovery_magic) + sizeof(chain_id_type);
constexpr size_t recovery_checksum_size = sizeof(fc::sha256);

struct recovered_block {
   uint32_t height;
   block_id_type id;
   signed_block_ptr block;
};
}

void reversible_recovery::sync_directory(const std::filesystem::path& directory) {
   const auto path = directory.empty() ? std::filesystem::path(".") : directory;
   const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
   EOS_ASSERT(fd >= 0, block_log_exception, "Cannot open recovery directory ${p}", ("p", directory));
   auto close_fd = fc::make_scoped_exit([&] { ::close(fd); });
   performance::scope timer(performance::stage::directory_sync);
   EOS_ASSERT(::fsync(fd) == 0, block_log_exception, "Cannot sync recovery directory ${p}", ("p", directory));
}

reversible_recovery::reversible_recovery(const std::filesystem::path& directory, const chain_id_type& chain_id)
   : directory(directory), chain_id(chain_id) {
   std::filesystem::create_directories(directory);
   sync_directory(directory.parent_path());
   // The reversible directory itself may have been created at the same time.
   sync_directory(directory.parent_path().parent_path());
}

std::vector<signed_block_ptr> reversible_recovery::load() {
   std::lock_guard lock(mutex);
   std::vector<recovered_block> recovered;
   for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      // A .tmp file was never committed and therefore cannot have authorized a vote.
      if (entry.path().extension() == ".tmp") continue;
      EOS_ASSERT(!entry.is_symlink() && entry.is_regular_file() && entry.path().extension() == ".block", block_log_exception,
                 "Unexpected file in reversible recovery directory: ${p}", ("p", entry.path()));
      std::ifstream input(entry.path(), std::ios::binary | std::ios::ate);
      EOS_ASSERT(input.good(), block_log_exception, "Cannot read recovery block ${p}", ("p", entry.path()));
      // Check the existing v1 size bound BEFORE allocation. One sized read avoids
      // per-byte stream iteration and repeated growth of the input buffer.
      const auto size = input.tellg();
      EOS_ASSERT(size > std::streamoff(recovery_prefix_size + recovery_checksum_size) &&
                 size <= std::streamoff(UINT32_MAX), block_log_exception,
                 "Truncated or oversized committed recovery block ${p}", ("p", entry.path()));
      std::vector<char> bytes(static_cast<size_t>(size));
      input.seekg(0, std::ios::beg);
      input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      EOS_ASSERT(input.good(), block_log_exception, "Error reading recovery block ${p}", ("p", entry.path()));
      // Do not silently accept an appended suffix if a file changes after tellg.
      EOS_ASSERT(input.peek() == std::char_traits<char>::eof() && !input.bad(), block_log_exception,
                 "Recovery block changed while reading ${p}", ("p", entry.path()));
      // Validate all bytes before deserializing corruption-controlled lengths.
      const size_t content_size = bytes.size() - recovery_checksum_size;
      fc::sha256 checksum;
      std::memcpy(checksum.data(), bytes.data() + content_size, sizeof(checksum));
      EOS_ASSERT(checksum == fc::sha256::hash(bytes.data(), content_size), block_log_exception,
                 "Recovery block checksum mismatch: ${p}", ("p", entry.path()));
      fc::datastream<const char*> ds(bytes.data(), content_size);
      uint64_t magic;
      auto stored_chain_id = chain_id_type::empty_chain_id();
      fc::raw::unpack(ds, magic);
      fc::raw::unpack(ds, stored_chain_id);
      EOS_ASSERT(magic == recovery_magic && stored_chain_id == chain_id, block_log_exception,
                 "Recovery format or chain ID mismatch: ${p}", ("p", entry.path()));
      auto block = std::make_shared<signed_block>();
      fc::raw::unpack(ds, *block);
      const auto id = block->calculate_id();
      const auto height = block->block_num();
      EOS_ASSERT(ds.remaining() == 0 && entry.path().stem().string() == id.str(),
                 block_log_exception, "Recovery block ID or length mismatch: ${p}", ("p", entry.path()));
      saved.emplace(id, height);
      recovered.push_back({height, id, std::move(block)});
   }
   // A block ID hashes its header. Compute it once per file, not twice per sort
   // comparison, while preserving the exact height/ID ordering of v1 recovery.
   std::sort(recovered.begin(), recovered.end(), [](const auto& a, const auto& b) {
      return std::tie(a.height, a.id) < std::tie(b.height, b.id);
   });
   std::vector<signed_block_ptr> blocks;
   blocks.reserve(recovered.size());
   for (auto& item : recovered) blocks.emplace_back(std::move(item.block));
   return blocks;
}

void reversible_recovery::store(const signed_block_ptr& block) {
   performance::scope total_timer(performance::stage::recovery_store);
   const auto id = block->calculate_id();
   const auto queued_at = performance::mark();
   std::lock_guard lock(mutex);
   performance::since(performance::stage::recovery_lock, queued_at);
   if (block->block_num() <= pruned_through || saved.contains(id)) return;
   const auto filename = id.str();
   const auto destination = directory / (filename + ".block");
   const auto temporary = directory / (filename + ".tmp");
   const auto pack_start = performance::mark();
   const auto& packed_block = block->packed_signed_block();
   // load() already requires the complete v1 record to fit in UINT32_MAX.
   EOS_ASSERT(packed_block.size() <= UINT32_MAX - recovery_prefix_size - recovery_checksum_size,
              block_log_exception, "Recovery block exceeds v1 size limit");
   std::array<char, recovery_prefix_size> prefix{};
   fc::datastream<char*> prefix_stream(prefix.data(), prefix.size());
   fc::raw::pack(prefix_stream, recovery_magic);
   fc::raw::pack(prefix_stream, chain_id);
   EOS_ASSERT(prefix_stream.remaining() == 0, block_log_exception, "Unexpected recovery v1 prefix size");
   std::vector<char> bytes;
   // Reserve the checksum too: appending it must not reallocate/copy a full block.
   bytes.reserve(prefix.size() + packed_block.size() + recovery_checksum_size);
   bytes.insert(bytes.end(), prefix.begin(), prefix.end());
   bytes.insert(bytes.end(), packed_block.begin(), packed_block.end());
   const auto checksum = fc::sha256::hash(bytes.data(), bytes.size());
   bytes.insert(bytes.end(), checksum.data(), checksum.data() + checksum.data_size());
   performance::since(performance::stage::recovery_pack, pack_start);
   fc::cfile file;
   file.set_file_path(temporary);
   file.open("wb");
   file.write(bytes.data(), bytes.size());
   file.flush();
   {
      performance::scope timer(performance::stage::recovery_file_sync);
      file.sync();
   }
   file.close();
   std::filesystem::rename(temporary, destination);
   sync_directory(directory);
   saved.emplace(id, block->block_num()); // only after the durable commit
}

void reversible_recovery::prune(uint32_t durable_lib) {
   performance::scope timer(performance::stage::recovery_prune);
   std::lock_guard lock(mutex);
   for (auto it = saved.begin(); it != saved.end();) {
      if (it->second <= durable_lib) {
         std::filesystem::remove(directory / (it->first.str() + ".block"));
         it = saved.erase(it);
      } else ++it;
   }
   sync_directory(directory);
   pruned_through = std::max(pruned_through, durable_lib);
}
} // namespace eosio::chain
