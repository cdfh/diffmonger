#ifndef DIFFMONGER_PIDOWNER_HPP
#define DIFFMONGER_PIDOWNER_HPP

#include <cstring>
#include <system_error>
#include <stdexcept>
#include <string>
#include <utility>

#include <unistd.h>
#include <sys/wait.h>

namespace diffmonger {

struct command_failed : std::runtime_error
{
    command_failed(int const wstatus, std::string name, std::string what)
        : std::runtime_error(name + ": " + what),
          wstatus(wstatus),
          name(name)
    {}

    /**
     * As set by waitpid(), without any macros applied.
     */
    int wstatus;

    std::string name;
};

class PidOwner
{
    static constexpr int empty_pid = -1;
    pid_t pid = empty_pid;

    /**
     * A string describing the process for user-facing messages.
     * This will typically be the name or path of the invoked executable,
     * but in the case of a PidOwner resulting from a fork() that does not subsequently
     * run exec(), this will be a descriptive name of the child process.
     */
    std::string name;

    /**
     * Stores the signal that will be given to the ::kill() syscall by the kill()
     * method when no signal is explicitly given.
     * A value of 0 (corresponding to SIGNULL) results in kill() being a noop.
     */
    int signal;

public:
    PidOwner() noexcept
    {}

    PidOwner(int const pid, std::string name = std::string{}, int const signal = 0)
        : pid(pid), name(std::move(name)), signal(signal)
    {
        if (pid <= 0)
            throw std::invalid_argument("invalid pid for " + this->name);
    }

    /**
     * Creates a PidOwner by forking,
     * returning an empty PidOwner in the child process and a populated
     * PidOwner in the parent process.
     */
    static PidOwner fork(std::string name = std::string{})
    {
        int const pid = ::fork();

        if (pid == -1)
            throw std::system_error(errno, std::system_category(), "fork failed");

        return pid ? PidOwner{pid, std::move(name)} : PidOwner{};
    }

    template <typename F>
    static PidOwner runInChild(F &&f, std::string name = std::string{})
    {
        auto pid = fork(std::move(name));

        if (!pid)
        {   // Child.
            try
            {
                std::forward<F>(f)();
                ::exit(0);
            } catch (std::runtime_error const &e)
            {
                fprintf(stderr, "Error: %s\n", e.what());
            } catch (std::logic_error const &e)
            {
                fprintf(stderr, "Logic error: %s\n", e.what());
            } catch (std::exception const &e)
            {
                fprintf(stderr, "Exception: %s\n", e.what());
            } catch (...)
            {
                fprintf(stderr, "Unknown error");
            }
            std::abort();
        }

        return pid;
    }

    /**
     * Move constructor.
     */
    PidOwner(PidOwner &&other) noexcept
    {
        swap(other);
    }

    /**
     * Move assignment.
     */
    PidOwner &operator=(PidOwner &&other) noexcept
    {
        if (&other != this)
            reset();
        swap(other);
        return *this;
    }

    PidOwner(PidOwner const &) = delete;
    PidOwner &operator=(PidOwner const &) = delete;

    /**
     * No equality operator.
     * This is because it would be awkwardly defined; does equality require \c name
     * and \c signal to also be identical, or just the pid?
     */
    bool operator==(PidOwner const &) = delete;

    void swap(PidOwner &other) noexcept
    {
        if (&other == this)
            return;

        std::swap(pid, other.pid);
        std::swap(name, other.name);
        std::swap(signal, other.signal);
    }

    int getPid() const
    {
        ensureNotEmpty("getPid");

        return pid;
    }

    char const *getName(char const *default_name = "unnamed process") const
    {
        ensureNotEmpty("getName");

        return name.empty() ? default_name : name.c_str();
    }

    operator bool() const
    {
        return !isEmpty();
    }

    bool isEmpty() const
    {
        return pid == empty_pid;
    }

    /**
     * Like reset(), but using the stored signal instead of a given signal.
     * If it is intended to make the current instance empty by simplying
     * wait()ing on the process without first killing, then use wait().
     */
    void reset()
    {
        reset(signal);
    }

    /**
     * Kill the underlying process with the given signal and then wait() for it,
     * leaving the object in an empty state once done.
     *
     * The exception safety of this method is naunced.
     * The method itself has strong exception safety.
     * However, if it throws, then necessarily, the instance shall still not
     * be empty after throwing, and whatever the error that triggered the exception was,
     * it will reoccur upon destruction
     * (other than if the error was due to an invalid signal being given),
     * when the destructor calls reset().
     * Thus, an exception (other than due to an invalid signal)
     * will typically imply that a std::terminate() will follow shortly.
     * If the caller is somehow in a position to correct the error,
     * then it should call release() and deal with the situation directly.
     */
    void reset(int const signal)
    {
        if (isEmpty())
            return;

        // Note: another implementation would swap with a temporary PidOwner instance,
        // leveraging the destructor to do the kill()ing and wait()ing.
        // However, the destructor is noexcept, so doing that would result in the current
        // method calling std::terminate() on any error.
        kill(signal);
        wait();
    }

    /**
     * Resets to empty by patiently wait()ing on the pid without killing it.
     * Returns the exit code of the underlying process.
     */
    int wait()
    {
        ensureNotEmpty("wait");

        int wstatus;
        int r;
        do
            r = ::waitpid(pid, &wstatus, 0);
        while ((r == -1) && (errno == EINTR));

        if (r == -1)
            // This is a logic error not a runtime error:
            // the only errors waitpid() will return are either the child does not exist
            // (i.e., it has already been wait()ed on; a logic error)
            // or the options argument was invalid
            // (which it's not, since no options were given...).
            throw std::logic_error("Invalid waitpid() for " + name + ": " + strerror(errno));

        // Up until this point, no members have been mutated.
        // After this point, members get mutated and so, for exception safety,
        // the remaining code must be noexcept.
        pid = empty_pid;

        return wstatus;
    }

    void wait_or_throw()
    {
        int const wstatus = wait();

        if (!WIFEXITED(wstatus) || (WEXITSTATUS(wstatus) != 0))
            throw command_failed(
                wstatus, name,
                WIFSIGNALED(wstatus)
                ? std::string("command failed due to signal: ").append(
                    [] (auto *desc) {
                        return desc ? std::string("SIG").append(desc) : "unknown signal";
                    }(sigabbrev_np(WTERMSIG(wstatus))))
                : "command failed with exit code: " + std::to_string(WEXITSTATUS(wstatus)));
    }

    /**
     * Return the underlying pid and then release ownership of it
     * so that the PidOwner object is now empty.
     */
    int release()
    {
        ensureNotEmpty("release");

        return std::exchange(pid, empty_pid);
    }

    void kill(int const signal = SIGKILL) const
    {
        ensureNotEmpty("kill");

        // signal == 0: SIGNULL: do nothing
        if (!signal)
            return;

        int const r = ::kill(pid, signal);

        if (r == -1)
        {
            if (errno == EINVAL)
                throw std::invalid_argument(
                    "PidOwner::kill(): invalid signal: " + std::to_string(signal));
            else
                throw std::system_error(errno, std::system_category(),
                                        std::string("kill(")
                                        .append(std::to_string(pid))
                                        .append(", ")
                                        .append(std::to_string(signal))
                                        .append(")"));
        }
    }

    ~PidOwner()
    {
        reset();
    }
protected:
    void ensureNotEmpty(char const * const method) const
    {
        if (isEmpty())
            throw std::logic_error(
                std::string("PidOwner::")
                .append(method).append("(): called on empty PidOwner"));
    }
};

}

#endif
