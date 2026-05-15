#ifndef LIBS_UTIL_INCLUDE_DIFFMONGER_UTIL_IOUTIL_HPP
#define LIBS_UTIL_INCLUDE_DIFFMONGER_UTIL_IOUTIL_HPP

#include <diffmonger/util/FdOwner.hpp>

#include <span>
#include <vector>
#include <filesystem>
#include <expected>

namespace diffmonger {
namespace ioutil {
/**
 * The same as write(), but guarantees that the data shall not be copied to userspace
 * memory (it's obviously the caller's responsibility to ensure the memory pointed
 * to by the span is secure).
 * At present, write() and write_secure() are the same function,
 * but they could feasibly differ in future if ever write() starts buffering to reduce
 * syscall overhead. Having an explicit write_secure() function provides forward
 * guarantees as to safety without committing the write() function to the same guarantees.
 */
void write_secure(FdOwner const &fd_, std::span<std::byte const> data);
void write_secure2(FdOwner fd, std::span<std::byte const> data);

/**
 * Writes the entirety of the given data to the fd.
 * Notes:
 *   - the fd must not be non-blocking.
 *   - does not close() the fd or make the FdOwner instance empty.
 */
void write(FdOwner const &fd, std::span<std::byte const> data);

/**
 * Convenience wrapper for write() that accepts the fd by value so that the fd
 * gets close()ed upon returning.
 */
void write2(FdOwner fd, std::span<std::byte const> data);


/**
 * Read the entirety of the fd into a buffer and return said buffer.
 * Notes:
 *   - the fd must not be non-blocking.
 *   - does not close() the fd or make the FdOwner instance empty.
 */
std::vector<std::byte> read(FdOwner const &fd, size_t bufsize = 4096);

/**
 * Convenience wrapper for read() that accepts the fd by value so that the fd
 * gets close()ed upon returning.
 */
std::vector<std::byte> read2(FdOwner fd);

/**
 * Reads from the given FdOwner into the given buffer until either the buffer
 * is full or EOF, returning the number of bytes that were read.
 * If the return value is less than buffer.size(), then EOF occurred.
 */
[[nodiscard]] size_t readBytes(FdOwner const &fd, std::span<std::byte> buffer);

/**
 * Like readBytes(), but reads exactly the number of bytes in the buffer,
 * or throws a std::runtime_error with the given message.
 */
void readBytesExact(FdOwner const &fd,
                    std::span<std::byte> buffer,
                    std::string_view what = "Unexpected EOF");

template <typename F>
void consume(FdOwner const &fd, std::span<std::byte> const buffer, F const &f)
{
    while (true)
    {
        size_t const nread = readBytes(fd, buffer);
        if (nread)
            f(std::span(buffer.data(), nread));
        if (nread != buffer.size())
            return;
    }
}

template <typename F>
void consume2(FdOwner fd, std::span<std::byte> const buffer, F const &f)
{
    consume(fd, buffer, f);
}

std::vector<std::byte> readfile(std::filesystem::path const &path);

/**
 * Try to read a file, or return the std::system_error that would otherwise
 * be thrown, with the code appropriately set.
 * A code of ENOENT indicates no such file.
 */
std::expected<std::vector<std::byte>, std::system_error>
tryReadFile(std::filesystem::path const &path);

void writefile(std::filesystem::path const &path,
               std::span<std::byte const> const bytes,
               int mode,
               bool must_create = false);

template <typename T, typename ...Params>
void writefile(std::filesystem::path const &path,
               std::vector<T> const &bytes,
               Params &&...params)
{
    writefile(path, std::as_bytes(std::span(bytes)), std::forward<Params>(params)...);
}

#if 0
struct Buffered
{
    std::vector<std::byte> buffer;
    std::vector<std::byte>::iterator data_begin;
    std::vector<std::byte>::iterator data_end;

    Buffered(size_t bufsize)
    {
        buffer.resize(bufsize);
    }

    void readBytesExact(FdOwner const &fd,
                        std::span<std::byte> out,
                        std::string_view what = "Unexpected EOF")
    {
        size_t nout = 0;
        bool eof = false;

        while (!eof && (nout != out.size()))
        {
            if (data_begin == data_end)
            {
                data_begin = buffer.begin();
                data_end = data_begin + readBytes(fd, buffer);

                eof = (data_end - data_begin) != buffer.size();
            }

            size_t const n = std::min(size_t(data_end - data_begin),
                                      out.size() - nout);
            memcpy(out.data() + nout, &*data_begin, n);
            data_begin+= n;
            nout += n;
        }

        if (nout != out.size())
            throw std::runtime_error(std::string(what));
    }

};
#endif

}}

#endif
