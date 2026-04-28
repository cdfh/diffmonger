#include <diffmonger/util/Uuid.hpp>
#include <diffmonger/util/FdOwner.hpp>
#include <diffmonger/util/ioutil.hpp>

#include <fcntl.h>
#include <unistd.h>

namespace diffmonger {

Uuid Uuid::random()
{
    auto fd = FdOwner::from_syscall(open("/dev/urandom", O_RDONLY), "open", "/dev/urandom");
    std::array<std::byte, size> bytes;
    ioutil::readBytesExact(fd, bytes);
    return Uuid{bytes};
}

void Uuid::ascii(std::string &out) const
{
    static constexpr char hex[] = "0123456789abcdef";

    out.reserve(out.size() + size*2);

    for (auto const b: bytes)
    {
        auto const v = std::to_integer<unsigned>(b);

        out.push_back(hex[(v >> 4) & 0xf]);
        out.push_back(hex[v & 0xf]);
    }
}


}
