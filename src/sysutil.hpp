#ifndef DIFFMONGER_SYSUTIL_HPP
#define DIFFMONGER_SYSUTIL_HPP

#include <diffmonger/util/FdOwner.hpp>
#include <diffmonger/util/PidOwner.hpp>

#include <unistd.h>

#include <span>
#include <string>
#include <vector>
#include <span>

namespace diffmonger {

inline auto with_null_terminated_array_of_cstrs(std::span<std::string const> const vec,
                                                auto &&f)
{
    std::vector<char const *> tmp;

    for (auto const &x: vec)
        tmp.push_back(x.c_str());

    tmp.push_back(nullptr);

    return f(std::move(tmp));
}

inline auto with_null_terminated_array_of_cstrs2(std::span<std::string const> const vec,
                                                 auto &&f)
{
    return with_null_terminated_array_of_cstrs(
        vec,
        [&] (auto const &args) { return f(args.data()); });
}

/**
 * Calls the given path with the given arguments and the given FdOwners,
 * returning a PidOwner for the caller to wait on.
 */
PidOwner systemvp3(char const *path,
                   char const *const *argv,
                   FdOwner stdin = FdOwner{},
                   FdOwner stdout = FdOwner::dup(STDOUT_FILENO),
                   FdOwner stderr = FdOwner::dup(STDERR_FILENO));

void systemvp_throw_on_failure3(char const *path,
                                char const *const *argv,
                                // Okay for this to have O_CLOEXEC.
                                FdOwner stdin,
                                FdOwner stdout = FdOwner::dup(STDOUT_FILENO),
                                FdOwner stderr = FdOwner::dup(STDERR_FILENO));

inline void systemvp_throw_on_failure(char const *path,
                                      char const *const *argv,
                                      FdOwner stdout = FdOwner::dup(STDOUT_FILENO),
                                      FdOwner stderr = FdOwner::dup(STDERR_FILENO))
{
    systemvp_throw_on_failure3(path, argv, FdOwner{}, std::move(stdout), std::move(stderr));
}

std::string systemvp_str(char const *path,
                         char const *const *argv,
                         FdOwner stderr = FdOwner::dup(STDERR_FILENO));

/**
 * The same as systemvp_str(), but removes the last character if it is a newline.
 */
std::string systemvp_strchomp(char const *path,
                              char const *const *argv,
                              FdOwner stderr = FdOwner::dup(STDERR_FILENO));

std::string concat_interspersed(char const *const *argv, char const *const separator = " ");

}
#endif
