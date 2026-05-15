#ifndef DIFFMONGER_REPOSITORYPARAMS_HPP
#define DIFFMONGER_REPOSITORYPARAMS_HPP

#include <diffmonger/util/FdOwner.hpp>

#include <memory>

namespace diffmonger {

class RepositoryStructure;

struct RepositoryParams
{
    bool encryption;
    size_t min_seconds_between_snapshots;

    // Throws InvalidParams
    void verify() const;
};

std::unique_ptr<RepositoryParams> createRepositoryParams(RepositoryStructure const &);

}

#endif
