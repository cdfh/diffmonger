#include <diffmonger/util/FdOwner.hpp>

#include <unistd.h>
#include <fcntl.h>

namespace diffmonger {

/**
 * Creates a pipe pair, with both fds having the given flags set.
 * Note that O_CLOEXEC does not persist following a dup() or dup2().
 */
FdPipePair FdPipePair::create(int const flags)
{
    int fds[2];

    int const r = pipe2(fds, flags);
    if (r == -1)
        throw std::system_error(errno, std::system_category(), "pipe() failed");

    return { .read_end = FdOwner{ fds[0] }, .write_end = FdOwner{ fds[1] } };
}


FdOwner from_syscall(int const fd, char const * const what)
{
    if (fd == -1)
        throw std::system_error(errno,
                                std::system_category(),
                                what);
    return FdOwner{fd};
}


FdOwner FdOwner::dup(int const fd)
{
    return from_syscall(::dup(fd),
                        "dup() failed for given fd",
                        std::to_string(fd));
}

FdOwner FdOwner::dup() const
{
    ensureNotEmpty("dup");
    return FdOwner::dup(fd);
}

FdOwner FdOwner::dup_close_on_exec() const
{
    ensureNotEmpty("dup_close_on_exec");

    auto const fd2 = fcntl(fd, F_DUPFD_CLOEXEC, 0);

    if (fd2 == -1)
        throw std::system_error(
            errno, std::system_category(),
            "FdOwner::dup_close_on_exec: fcntl(fd, F_DUPFD_CLOEXEC, 0) failed");

    return FdOwner{fd2};
}

bool FdOwner::is_close_on_exec() const
{
    int const flags = fcntl(fd, F_GETFD);
    if (flags == -1)
        throw std::system_error(errno, std::system_category(),
                                "fcntl");
    return flags&FD_CLOEXEC;
}


void FdOwner::reset()
{
    if (isEmpty())
        return;

    int const r = ::close(fd);

    if ((r == 0) || ((r == -1) && (errno == EINTR)))
    {
        // Failure due to EINTR is fine;
        // Linus says not to re-try close() if it fails due to EINTR
        // because the fd will nevertheless be closed:
        // http://lkml.indiana.edu/hypermail/linux/kernel/0509.1/0877.html
        fd = empty_fd;
    } else // Do /not/ reset fd to empty if close() failed!
    {
        if (errno == EBADF)
            // Presumably this object was constructed from an invalid fd
            // (i.e., one that was already close()ed or that had never been open()ed).
            // This is a logic error.
            throw std::logic_error(std::string("FdOwner::reset(): fd (")
                                   .append(std::to_string(fd))
                                   .append(") is invalid"));
        // If close() fails for another reason, do not throw;
        // it's possibly a failing in the OS or fs rather than an issue related to
        // the calling code.
    }
}

void FdOwner::mov2(int const target_fd)
{
    ensureNotEmpty("mov2");

    // The actual dup2() would be a no-op, but semantically, the caller has taken
    // ownership of the fd by assigning it to the raw integer target_fd.
    // Thus, we call release() to release ownership without close()ing.
    if (fd == target_fd)
        release();

    if (::dup2(fd, target_fd) == -1)
        throw std::system_error(errno, std::system_category(), "FdOwner::mov2: dup2 failed");

    reset();
}

void FdOwner::ensureNotEmpty(char const * const method) const
{
    if (isEmpty())
        throw std::logic_error(
            std::string("FdOwner::")
            .append(method).append("(): called on empty FdOwner"));
}


}
