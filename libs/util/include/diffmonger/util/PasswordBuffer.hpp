#ifndef DIFFMONGER_UTIL_PASSWORDBUFFER_HPP
#define DIFFMONGER_UTIL_PASSWORDBUFFER_HPP

#include <span>
#include <string_view>
#include <memory>
#include <filesystem>

// TODO: See libsodium's docs on sodium_malloc():
//   https://libsodium.gitbook.io/doc/memory_management
// In particular, sodium puts guard pages before and after the allocated memory;
// attempts to read data from the guard pages result in a fault.
// This reduces risk of sensitive data being copied by reading off the end of
// buffers. Obviously, this isn't useful in the current application,
// because diffmonger has no buffer overruns!

namespace diffmonger {

class FdOwner;

class PasswordBuffer
{
public:
    enum class TolerateSwapEnabled
    {
        False = 0,
        True,
    };

    enum class TolerateCoreDumps
    {
        False = 0,
        True,
    };

    struct Options
    {
        TolerateSwapEnabled tolerateSwapEnabled = TolerateSwapEnabled::False;
        TolerateCoreDumps tolerateCoreDumps = TolerateCoreDumps::False;
        /**
         * Undo the effects of MADV_DODUMP.
         * This is disabled by default because there's no way to know whether
         * calling code marked memory regions as MADV_DODUMP prior to and independently
         * to the constructor doing so. For all we know,
         * main() explicitly marked the entire address space as MADV_DODUMP,
         * in which case it would be terrible to undo that.
         * In the current application, the user is likely to only be asked for at most two
         * passwords in the lifetime of the program, and so there's essentially
         * zero downside to failing to mark regions as DODUMP,
         * but there are potential real security issues with doing so.
         * Ditto for unlock_on_destruction.
         */
        bool dodump_on_destruction = false;
        bool unlock_on_destruction = false;

        Options()
            : tolerateSwapEnabled{TolerateSwapEnabled::False},
              tolerateCoreDumps{TolerateCoreDumps::False}
        {}

        auto operator<=>(Options const &) const = default;
    };

    PasswordBuffer(size_t length,
                   Options options = Options{});

    /**
     * Delete all other constructors, including default and move constructors.
     * These would not be hard to write,
     * but deleting them in favour of client code using std::unique_ptr in their place
     * reduces the surface area for bugs without any drawbacks,
     * which is particularly beneficial given the sensitivity of storing passwords.
     */
    PasswordBuffer() = delete;
    PasswordBuffer(PasswordBuffer &&) = delete;
    PasswordBuffer &operator=(PasswordBuffer &&) = delete;

    std::unique_ptr<PasswordBuffer> clone() const;

    static std::unique_ptr<PasswordBuffer> readline(
        FdOwner fd, Options options = Options{});

    // Read until the first occurrence of sentinel,
    // or until EOF if sentinel is empty.
    // May read beyond the EOF, in which case extra chars are discarded.
    static std::unique_ptr<PasswordBuffer> readuntil(
        FdOwner fd,
        std::span<char const> sentinel = {},
        Options options = Options{});

    // TODO: Use pinentry(1) for this.
    static std::unique_ptr<PasswordBuffer> readPasswordFromTty(
        std::string_view prompt,
        std::filesystem::path tty = "/dev/tty",
        Options options = Options{});

    void write(FdOwner const &fd) const;

    size_t getSize() const { return length; }

    char const *getPointer() const { return password; }
    char *getWritePointer() { return password; }

    std::span<char const> getSpan() const { return std::span(getPointer(), getSize()); }

    // For libsodium... The reinterpret_cast is safe here; the standard allows it.
    unsigned char const *getUPointer() const
    { return reinterpret_cast<unsigned char const *>(getPointer()); }
    unsigned char *getWriteUPointer()
    { return reinterpret_cast<unsigned char*>(getWritePointer()); }

    ~PasswordBuffer();

protected:
    std::unique_ptr<PasswordBuffer> resizedTo(size_t const newlen) const;

    // Ensure the current instance would have been successfully constructed
    // had it been constructed with the given options.
    void ensureCompatible(Options options) const;

    void clear() noexcept;

private:
    /**
     * Unaligned storage large enough to hold the password when aligned to page boundaries.
     * Page alignment is needed for madvise().
     */
    std::unique_ptr<char[]> bytes;

    /**
     * Pointer to page-aligned start of password
     * (which, due to alignment,  may not be start of \c bytes).
     * The password can contain arbitrary data; it is not null terminated.
     */
    char *password;
    /**
     * Password length.
     */
    size_t length = 0;

    Options options;

    bool unlock_required = false;
    bool dodump_required = false;
};

}

#endif
