







#pragma once

#include <sluice/measurement.hpp>
#include <sluice/reader.hpp>
#include <sluice/sync.hpp>
#include <sluice/writer.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace sluice {

class FileReader final : public Reader {
  public:
    FileReader() = default;




    explicit FileReader(const std::string& path, SyscallStats* stats = nullptr,
                        VectorStats* vec_stats = nullptr);


    explicit FileReader(int fd) : fd_(fd) {}
    ~FileReader() override;
    FileReader(FileReader&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)), open_error_(std::exchange(other.open_error_, {})),
          stats_(std::exchange(other.stats_, nullptr)),
          vec_stats_(std::exchange(other.vec_stats_, nullptr)) {}
    FileReader& operator=(FileReader&& other) noexcept {
        if (this != &other) {



            (void)close();
            fd_ = std::exchange(other.fd_, -1);
            open_error_ = std::exchange(other.open_error_, {});
            stats_ = std::exchange(other.stats_, nullptr);
            vec_stats_ = std::exchange(other.vec_stats_, nullptr);
        }
        return *this;
    }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    bool opened() const { return fd_ >= 0; }



    const std::optional<IoError>& open_error() const { return open_error_; }





















    Result<void> close() noexcept;


    Result<std::size_t> read_some(std::span<std::byte> dst) override;



    Result<std::size_t> read_vec(std::span<IoSlice> dsts) override;




    Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> dst);



    Result<std::size_t> read_vec_at(std::uint64_t offset, std::span<IoSlice> dsts);



    Result<void> read_at_exact(std::uint64_t offset, std::span<std::byte> dst);

  private:
    int fd_ = -1;


    std::optional<IoError> open_error_;


    SyscallStats* stats_ = nullptr;
    VectorStats* vec_stats_ = nullptr;
};

class FileWriter final : public Writer, public SyncableWriter {
  public:
    FileWriter() = default;





    explicit FileWriter(const std::string& path, SyscallStats* stats = nullptr,
                        VectorStats* vec_stats = nullptr, SyncStats* sync_stats = nullptr);


    explicit FileWriter(int fd) : fd_(fd) {}
    ~FileWriter() override;
    FileWriter(FileWriter&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)), open_error_(std::exchange(other.open_error_, {})),
          stats_(std::exchange(other.stats_, nullptr)),
          vec_stats_(std::exchange(other.vec_stats_, nullptr)),
          sync_stats_(std::exchange(other.sync_stats_, nullptr)) {}
    FileWriter& operator=(FileWriter&& other) noexcept {
        if (this != &other) {



            (void)close();
            fd_ = std::exchange(other.fd_, -1);
            open_error_ = std::exchange(other.open_error_, {});
            stats_ = std::exchange(other.stats_, nullptr);
            vec_stats_ = std::exchange(other.vec_stats_, nullptr);
            sync_stats_ = std::exchange(other.sync_stats_, nullptr);
        }
        return *this;
    }
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    bool opened() const { return fd_ >= 0; }



    const std::optional<IoError>& open_error() const { return open_error_; }









    Result<void> close() noexcept;


    Result<std::size_t> write_some(std::span<const std::byte> src) override;



    Result<std::size_t> write_vec(std::span<const ConstIoSlice> srcs) override;




    Result<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> src);



    Result<std::size_t> write_vec_at(std::uint64_t offset, std::span<const ConstIoSlice> srcs);



    Result<void> write_at_all(std::uint64_t offset, std::span<const std::byte> src);




    Result<void> flush() override { return {}; }


    Result<void> sync_data() override;

    Result<void> sync_all() override;

  private:
    int fd_ = -1;
    std::optional<IoError> open_error_;
    SyscallStats* stats_ = nullptr;
    VectorStats* vec_stats_ = nullptr;
    SyncStats* sync_stats_ = nullptr;
};

}
