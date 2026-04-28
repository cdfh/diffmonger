#include "sysutil.hpp"

#include <diffmonger/util/FdOwner.hpp>
#include <diffmonger/util/ioutil.hpp>
#include <diffmonger/util/PidOwner.hpp>

#include <cstddef>
#include <cassert>
#include <ctime>
#include <cerrno>

#include <span>
#include <string>

#include <err.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

namespace diffmonger {

PidOwner systemvp3(char const *path,
                   char const *const * const argv,
                   FdOwner stdin,
                   FdOwner stdout,
                   FdOwner stderr)
{
    // Important: must exist _before_ fork()!
    // Note: eventfd() would be much better, but it's Linux specific.
    auto pipepair = FdPipePair::create();

    auto child = PidOwner::runInChild(
        [&]
        {
            stdin.mov2_or_close(STDIN_FILENO);
            stdout.mov2(STDOUT_FILENO);
            stderr.mov2(STDERR_FILENO);

            execvp(const_cast<char *>(path), const_cast<char **>(argv));

            // Something went wrong... Tell the parent about it.

            std::byte errno_value[sizeof(errno)];

            memcpy(errno_value, &errno, sizeof(errno));

            pipepair.read_end.reset();
            ioutil::write(pipepair.write_end, std::span(errno_value, sizeof(errno)));

            // If execvp returns, it failed.
            // Terminate instead of throw because we're still in the child.
            err(EXIT_FAILURE, "%s %s", "Failed to run", argv[0]);
        },
        concat_interspersed(argv));

    // Parent. Close all fds (except the read end of the pipe),
    // they are for the child and not for the parent.
    stdin.reset();
    stdout.reset();
    stderr.reset();

    // Note: if execvp() succeeded, then child's write_end was close()ed due to O_CLOEXEC,
    // so we'll get an immediate EOF.
    pipepair.write_end.reset();

    if (auto const pipedata = ioutil::read2(std::move(pipepair.read_end)); !pipedata.empty())
    {
        int errno_value;
        memcpy(&errno_value, pipedata.data(), pipedata.size());
        throw std::system_error(errno_value, std::system_category(),
                                "Failed to run " + std::string(path));
    }

    return child;
}

std::string systemvp_str(char const *path, char const *const * const argv, FdOwner stderr)
{
    auto pipe = FdPipePair::create();

    PidOwner child = systemvp3(path,
                               argv,
                               FdOwner{},
                               std::move(pipe.write_end),
                               std::move(stderr));

    auto const bytes =
        ioutil::read2(std::move(pipe.read_end));

    child.wait_or_throw();

    return std::string(reinterpret_cast<char const *>(bytes.data()),
                       bytes.size());
}

void systemvp_throw_on_failure3(char const *path, char const *const * const argv,
                                FdOwner stdin, FdOwner stdout, FdOwner stderr)
{
    PidOwner child =
        systemvp3(path, argv, std::move(stdin), std::move(stdout), std::move(stderr));
    child.wait_or_throw();
}

std::string systemvp_strchomp(char const *path,
                              char const *const * const argv,
                              FdOwner stderr)
{
    auto out = systemvp_str(path, argv, std::move(stderr));

    if (!out.empty() && (out.back() == '\n'))
        out.pop_back();

    return out;
}


std::string concat_interspersed(char const *const *argv, char const *const separator)
{
    if (!argv || !*argv)
        throw std::invalid_argument("args_to_string: argv empty");

    std::string out = *argv++;

    while (*argv)
        out.append(separator).append(*argv++);

    return out;
}

}
