#ifndef DIFFMONGER_REPOSITORYPARAMS_HPP
#define DIFFMONGER_REPOSITORYPARAMS_HPP

#include <diffmonger/util/FdOwner.hpp>

#include <filesystem>
#include <memory>

namespace diffmonger {

class RepositoryStructure;

struct RepositoryParams
{
    bool encryption;
    std::filesystem::path repository;
    size_t min_seconds_between_snapshots;

    // Throws InvalidParams
    void verify() const;
};

/* TODO: Ideally this would only accept a vector of args (initargs)
 * and would be agnostic to the RepositoryStructure.
 * However, I want to override the R parameter of the diffmonger command
 * so it points to the actual repository.
 *
 * Note: while declared here, the definition is in CommandLineInterface.cpp,
 * as internally, createRepositoryParams() needs to parse the initargs.
 * Arguably the plumbing for doing this should be exposed in CommandLineInterface.hpp
 * so that RepositoryParams.cpp can do it for itself, but meh...
 */
std::unique_ptr<RepositoryParams> createRepositoryParams(RepositoryStructure const &);

}

#endif
