#include <diffmonger/util/ioutil.hpp>
#include <diffmonger/util/FdOwner.hpp>

#include <unistd.h>
#include <fcntl.h>

#include <memory>
#include <cassert>


namespace diffmonger {
namespace ioutil {

void write_secure(FdOwner const &fd, std::span<std::byte const> data)
{
    while (!data.empty())
    {
        ssize_t const nwritten = ::write(fd.get(), data.data(), data.size());

        if (nwritten < 0)
        {
            if (errno == EINTR)
                continue;
            throw std::system_error(errno, std::system_category(), "write() failed");
        } else
            data = { data.begin() + nwritten, data.end() };
    }
}

void write_secure2(FdOwner fd, std::span<std::byte const> data)
{
    return write_secure(fd, data);
}

void write(FdOwner const &fd, std::span<std::byte const> const data)
{
    return write_secure(fd, data);
}

void write2(FdOwner fd, std::span<std::byte const> data)
{
    write(fd, data);
}

std::vector<std::byte> read(FdOwner const &fd, size_t const bufsize)
{
    std::vector<std::byte> out;
    std::vector<std::byte> buf(bufsize);
    consume(fd, buf,
            [&out] (std::span<std::byte> bytes)
            {
                out.insert(out.end(), bytes.begin(), bytes.end());
            });
    return out;
}

std::vector<std::byte> read2(FdOwner fd)
{
    return read(fd);
}

[[nodiscard]] size_t readBytes(FdOwner const &fd, std::span<std::byte> const buffer)
{
    std::byte *ptr = buffer.data();
    std::byte *const end = ptr + buffer.size();

    while (ptr != end)
    {
        assert(ptr < end);

        ssize_t const nread = ::read(fd.get(), ptr, end - ptr);

        if (nread < 0)
        {
            if (errno == EINTR)
                continue;
            throw std::system_error(errno, std::system_category(), "read() failed");
        } else if (nread == 0)
        { // EOF
            return ptr - buffer.data();
        } else
        {
            ptr += nread;
        }
    }

    return ptr - buffer.data();
}

void readBytesExact(FdOwner const &fd,
                    std::span<std::byte> buffer,
                    std::string_view const what)
{
    size_t const nbytes = readBytes(fd, buffer);
    if (nbytes != buffer.size())
        throw std::runtime_error(std::string(what));
}

std::vector<std::byte> readfile(std::filesystem::path const &path)
{
    FdOwner fd = FdOwner::from_syscall(open(path.native().c_str(),
                                            O_RDONLY),
                                       "open",
                                       path.native());

    return read(std::move(fd));
}

std::expected<std::vector<std::byte>, std::system_error>
tryReadFile(std::filesystem::path const &path)
{
    int const fd = open(path.native().c_str(), O_RDONLY);

    if (fd != -1)
        return { read(FdOwner{fd}) };

    return std::unexpected{ std::system_error(errno,
                                              std::system_category(),
                                              std::string{}
                                              .append("Could not read file (")
                                              .append(path.native())
                                              .append(")")) };
}

void writefile(std::filesystem::path const &path,
               std::span<std::byte const> const bytes,
               int mode,
               bool must_create)
{
    int const flags = O_WRONLY | O_CREAT |
        (must_create ? O_EXCL : O_TRUNC);

    FdOwner fd = FdOwner::from_syscall(open(path.native().c_str(), flags, mode),
                                       "open",
                                       path.native());

    write(std::move(fd), bytes);
}

}}
