















#pragma once

#include <sluice/reader.hpp>
#include <sluice/sync.hpp>
#include <sluice/writer.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sluice::wal {

inline constexpr std::uint32_t magic = 0x57414CU;


Result<void> write_record(Writer& writer, std::span<const std::byte> payload);






Result<void> write_record_vec(Writer& writer, std::span<const std::byte> payload);



Result<std::vector<std::byte>> read_record(Reader& reader);







class WalWriter {
  public:

    explicit WalWriter(Writer& writer);


    WalWriter(Writer& writer, SyncableWriter* syncable);


    Result<void> write_record(std::span<const std::byte> payload);

    Result<void> write_record_vec(std::span<const std::byte> payload);


    Result<void> flush();




    Result<void> sync();

    std::uint64_t written_lsn() const noexcept { return written_lsn_; }
    std::uint64_t flushed_lsn() const noexcept { return flushed_lsn_; }
    std::uint64_t durable_lsn() const noexcept { return durable_lsn_; }

  private:
    Writer& writer_;
    SyncableWriter* syncable_;
    std::uint64_t written_lsn_ = 0;
    std::uint64_t flushed_lsn_ = 0;
    std::uint64_t durable_lsn_ = 0;
};

namespace detail {




Result<std::uint32_t> checked_u32_len(std::size_t len);




std::size_t read_chunk_size(std::size_t remaining) noexcept;

}

}
