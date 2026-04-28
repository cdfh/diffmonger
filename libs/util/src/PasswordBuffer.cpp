#include <diffmonger/util/PasswordBuffer.hpp>
#include <diffmonger/util/FdOwner.hpp>
#include <diffmonger/util/ioutil.hpp>

#include <iostream>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <cstring>
#include <string_view>
#include <cerrno>
#include <memory>
#include <span>
#include <algorithm>

namespace diffmonger {

namespace {

// Note: on x86, a page will always be 4k.
long getpagesize()
{
    long const x = sysconf(_SC_PAGESIZE);

    if (x == -1)
        throw std::system_error(errno, std::system_category(), "sysconf: _SC_PAGESIZE");

    return x;
}

}


PasswordBuffer::PasswordBuffer(size_t const length, Options const options)
{
    {
        long const pagesize = getpagesize();
        // +1 for the null terminator, -1 because we only need pagesize - 1 bytes to guarantee
        // alignment.
        size_t const nbytes = length + 1 + pagesize - 1;

        bytes = std::make_unique_for_overwrite<char[]>(nbytes);

        void *ptr = bytes.get();
        size_t tmpnbytes = nbytes;

        if (!std::align(pagesize, length + 1, ptr, tmpnbytes))
            throw std::logic_error("Could not align storage for password buffer");

        password = static_cast<char *>(ptr);
    }

    this->length = length;

    // Exclude memory from core dumps.
    if (madvise(password, length, MADV_DONTDUMP) == 0)
        dodump_required = options.dodump_on_destruction;
    else
    {
        if (options.tolerateCoreDumps == TolerateCoreDumps::False)
            throw std::system_error(errno, std::system_category(),
                                    "mdavise(,, MADV_DONTDUMP)");

        std::cout << "WARNING\n"
            "Unable to madvise the password in memory with the madvise() system call.\n"
            "This does not affect program correctness,\n"
            "but it does mean that your password may be saved in a core dump file.\n";
    }

    // Lock the buffer in memory. Do this /before/ reading the password!!
    // Note: the null terminator is not part of the password and doesn't get locked.
    if (mlock(password, length) == 0)
        unlock_required = options.unlock_on_destruction;
    else
    {
        if (options.tolerateSwapEnabled == TolerateSwapEnabled::False)
            throw std::system_error(errno, std::system_category(), "mlock");
        std::cout << "WARNING\n"
                "Unable to lock the password in memory with the mlock() system call.\n"
                "This does not affect program correctness,\n"
                "but it does mean that your password may be saved into swapspace.\n"
                "If your swapspace is encrypted, then this should not be an issue,\n"
                "but if your swapspace is /not/ encrypted, then you should\n"
                "abort the program and fix the issue.\n";
    }
}

PasswordBuffer::~PasswordBuffer()
{
    clear();

    if (unlock_required)
        if (munlock(password, length) != 0)
            perror("munlock");
    if (dodump_required)
        if (madvise(password, length, MADV_DODUMP) != 0)
            perror("madvise");
}

void PasswordBuffer::clear() noexcept
{
    // Note: intentionally do not zero or unlock the null terminator;
    // it does not hold anything secret.
    // See also: memset_s().
    ::explicit_bzero(password, length);
}

void PasswordBuffer::write(FdOwner const &fd) const
{
    ioutil::write_secure(fd, std::as_bytes(std::span(password, length)));
}

std::unique_ptr<PasswordBuffer>
PasswordBuffer::readuntil(FdOwner fd,
                          std::span<char const> const sentinel,
                          Options options)
{
    size_t const size0 = getpagesize()/2;

    std::unique_ptr<PasswordBuffer> buffer =
        std::make_unique<PasswordBuffer>(size0, options);

    size_t password_length = 0;
    bool done = false;
    while (!done)
    {
        if (password_length == buffer->getSize())
            buffer = buffer->resizedTo(buffer->getSize()*2);

        ssize_t const nread =
            ::read(fd.get(),
                   const_cast<char *>(buffer->password) + password_length,
                   buffer->getSize() - password_length);

        if (nread < 0)
        {
            if (errno == EINTR)
                continue;
            throw std::system_error(errno, std::system_category(), "read() failed");
        } else if (nread == 0)
        { // EOF
            done = true;
        } else if (sentinel.empty())
        {
            password_length += nread;
        } else
        {
            auto const *search_begin = buffer->password + password_length;
            auto const *search_end = search_begin + nread;
            auto const it = std::search(search_begin, search_end,
                                        sentinel.begin(), sentinel.end());
            done = it != search_end;
            password_length = it - buffer->password;
        }
    }

    buffer->password[password_length] = '\0';

    return buffer->resizedTo(password_length);
}

std::unique_ptr<PasswordBuffer> PasswordBuffer::readline(FdOwner fd,
                                                         PasswordBuffer::Options options)
{
    std::string_view sentinel = "\n";
    return readuntil(std::move(fd), {sentinel}, std::move(options));
}

std::unique_ptr<PasswordBuffer>
PasswordBuffer::readPasswordFromTty(
        std::string_view const prompt,
        std::filesystem::path const tty,
        Options const options)
{
    // See also: getpass(3) on Linux (unistd.h), but this is deprecated.
    class EchoDisabled
    {
        FdOwner tty;
    public:
        termios attr_orig;

        // Disables echo on TTY
        EchoDisabled(std::filesystem::path const &ttypath)
            : tty(FdOwner::from_syscall(
                      open(ttypath.c_str(), O_RDONLY),
                      "Failed to open given tty for reading password",
                      ttypath.native()))
        {
            if (tcgetattr(tty.get(), &attr_orig) != 0)
                throw std::system_error(errno, std::system_category(), "tcgetattr");

            termios attr = attr_orig;
            // Disable echo.
            attr.c_lflag &= ~ECHO;
            // Enable canonical mode (newline flushes input).
            attr.c_lflag |= ICANON;

            if (tcsetattr(tty.get(), TCSANOW, &attr) != 0)
                throw std::system_error(errno, std::system_category(), "tcsetattr");
        }

        auto duptty() const { return tty.dup(); }

        ~EchoDisabled()
        {
            tcsetattr(tty.get(), TCSANOW, &attr_orig);
            std::cout << std::endl;
        }
    } echoDisabled{tty};

    std::cout << prompt << std::flush;

    // Read the password.
    // Do not use buffered IO (fgets(), etc);
    // we do not want the password stored (even if only transiently) in additional buffers!
    return PasswordBuffer::readline(echoDisabled.duptty(), options);
}


std::unique_ptr<PasswordBuffer> PasswordBuffer::clone() const
{
    return resizedTo(length);
}

std::unique_ptr<PasswordBuffer> PasswordBuffer::resizedTo(size_t const newlen) const
{
    auto out = std::make_unique<PasswordBuffer>(newlen, options);
    size_t const n = std::min(newlen, length);
    for (size_t i=0; i != n; ++i)
        out->password[i] = password[i];
    out->password[n] = 0;
    return out;
}

}
