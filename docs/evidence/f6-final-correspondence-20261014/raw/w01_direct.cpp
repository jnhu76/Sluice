// F6 clean-room W-01 consumer: direct execution, no IoContext/worker/request
// table/progress source. Compiled against the installed prefix only.
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main() {
    using sluice::File;
    using sluice::FileAccess;
    using sluice::FileExistence;
    using sluice::FileInitialContents;
    using sluice::FileOpen;

    const char* path = "/tmp/f6_w01_probe.dat";
    std::vector<std::byte> out(12);
    std::memcpy(out.data(), "f6-w01-probe", 12);
    {
        auto f = File::open(path, FileOpen{
            FileAccess::write_only, FileExistence::create_if_missing,
            FileInitialContents::truncate});
        assert(f.has_value());
        auto w = sluice::blocking::write_at(f.value(), 0, out);
        assert(w.has_value() && w.value() == 12);
        auto s = sluice::blocking::sync_data(f.value());
        assert(s.has_value());
        auto c = f.value().close();
        assert(c.has_value());
    }
    {
        auto f = File::open(path);
        assert(f.has_value());
        auto info = sluice::blocking::file_info(f.value());
        assert(info.has_value());
        assert(info.value().size >= 12);
        std::vector<std::byte> in(12, std::byte{0});
        auto r = sluice::blocking::read_at(f.value(), 0, in);
        assert(r.has_value() && r.value() == 12);
        assert(std::string(reinterpret_cast<const char*>(in.data()), 12) ==
               "f6-w01-probe");
        auto c = f.value().close();
        assert(c.has_value());
        auto c2 = f.value().close();
        assert(c2.has_value());
    }
    std::remove(path);
    std::puts("F6_W01_DIRECT_CONSUMER_OK");
    return 0;
}
