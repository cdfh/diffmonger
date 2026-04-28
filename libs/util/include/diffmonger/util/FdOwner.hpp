#ifndef DIFFMONGER_FDOWNER_HPP
#define DIFFMONGER_FDOWNER_HPP

#include <fcntl.h>

#include <system_error>
#include <stdexcept>
#include <string>
#include <cerrno>
#include <utility>
#include <string_view>

namespace diffmonger {

/**
 * An RAII file descriptor owner that calls close() in the destructor.
 * Thread safety: not thread safe.
 */
class FdOwner
{
public:
    /**
     * Default constructor.
     * Constructs an empty instance.
     */
    FdOwner() noexcept {}

    /**
     * Constructor.
     * The given value must be a valid (i.e., not close()ed) file descriptor.
     * Once constructed, maintains the invariant that the instance is either
     * empty (isEmpty()) or the underlying fd valid (i.e., not close()ed).
     */
    explicit FdOwner(int const fd)
        : fd(fd >= 0 ? fd : throw std::invalid_argument("Invalid file descriptor"))
    {}

    /**
     * Wrapper around the constructor that assumes a given value of -1 implies that
     * \c errno is as set by open() (or creat(), openat(), openat2()) and throws
     * a \c std::system_error with this errno value.
     */
    static FdOwner from_syscall(int const fd, char const *what);

    /**
     * Convenience wrapper around from_syscall().
     */
    static FdOwner from_syscall(int const fd,
                                std::string_view const what,
                                std::string_view const specifically)
    {
        if (fd == -1)
            throw std::system_error(errno, std::system_category(),
                                    std::string(what)
                                    .append(" (")
                                    .append(specifically)
                                    .append(")"));
        return FdOwner{fd};
    }

    /**
     * Create an FdOwner by calling dup() on an existing fd.
     */
    static FdOwner dup(int const fd);

    /**
     * Move constructor.
     * Makes the moved-from fd empty.
     */
    FdOwner(FdOwner &&other) noexcept : fd(other.fd) { other.fd = empty_fd; }

    FdOwner(FdOwner const &) = delete;
    FdOwner &operator=(FdOwner const &) = delete;

    /**
     * Move assignment.
     * Be aware that if \c this is active before the call
     * and close() fails on the associated fd,
     * then this method will call std::terminate(). If this is not desired, call reset()
     * explicitly prior to the move assignment
     * (so that reset() can throw instead of terminating).
     *
     * However, terminating is probably the correct behaviour: close() should never fail,
     * other than if the fd was not open. If the fd was not open, then somehow \c this
     * has become invalid, which is now a logic error rather than a runtime error.
     */
    FdOwner &operator=(FdOwner &&other) noexcept
    {
        if (this != &other)
            reset();
        swap(other);
        return *this;
    }

    int get() const
    {
        ensureNotEmpty("get");

        return fd;
    }

    void swap(FdOwner &other) noexcept
    {
        std::swap(fd, other.fd);
    }

    FdOwner dup() const;

    /**
     * Returns a new FdOwner that has O_CLOEXEC set.
     * This is rather wasteful when a new FdOwner wasn't needed,
     * but it avoids the awkwardness of
     * having to get-then-set the flags with fcntl().
     */
    FdOwner dup_close_on_exec() const;

    bool is_close_on_exec() const;

    /**
     * Returns true if this FdOwner is empty or false otherwise.
     *
     * There's no implicit conversion to bool as erroneously passing an FdOwner instance
     * to a function that accepts a numeric fd argument would result in implicit
     * conversion of the FdOwner to an int via bool, which would very much not be the
     * intended effect.
     */
    bool isEmpty() const { return fd == empty_fd; }

    /**
     * Reset the (possibly empty) fd, calling close if the fd is not empty.
     * Strong exception safety: if this throws, then close() failed and the current
     * object is left unchanged.
     */
    void reset();

    /**
     * Release ownership of the underlying fd without calling close().
     * The FdOwner instance is empty following the call.
     */
    int release()
    {
        ensureNotEmpty("release");
        return std::exchange(fd, empty_fd);
    }

    /**
     * Moves the file descriptor to the given target file descriptor with dup2(),
     * calling reset() once done.
     * Note that the target_fd gets closed, if previously open (by virtue of dup2()).
     * Note that the O_CLOEXEC flag, if present, is /not/ preserved.
     * Exception safety: strong.
     */
    void mov2(int const target_fd);

    /**
     * Like mov2(), but if the current FdOwner is empty,
     * then close()es the target instead of dup()ing.
     */
    void mov2_or_close(int const target_fd)
    {
        if (isEmpty())
            FdOwner(target_fd).reset();
        else
            mov2(target_fd);
    }

    ~FdOwner()
    {
        reset();
    }

protected:
    void ensureNotEmpty(char const * const method) const;
private:
    static constexpr int empty_fd = -1;
    int fd = empty_fd;
};

struct FdPipePair {
    FdOwner read_end;
    FdOwner write_end;

    /**
     * Creates a pipe pair, with both fds having the given flags set.
     * Note that O_CLOEXEC does not persist following a dup() or dup2().
     */
    static FdPipePair create(int const flags = O_CLOEXEC);
};

}
#endif
