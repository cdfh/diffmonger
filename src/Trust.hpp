#ifndef DIFFMONGER_TRUST_HPP
#define DIFFMONGER_TRUST_HPP

#include "RepositoryStructure.hpp"
#include "BackendDriver.hpp"

namespace diffmonger::trust {

std::optional<RepositoryStructure::Breadcrumb> maybeUpdateBreadcrumb(
    RepositoryStructure const &repositoryStructure,
    RepositoryStructure::Breadcrumb const &breadcrumb,
    BackendDriver::SnapshotId::Encoded const &candidate,
    BackendDriver::SnapshotId::timestamp_t::duration
    const min_time_between_snapshots,
    BackendDriver::SnapshotId::timestamp_t::clock::time_point const &now);

}
#endif
