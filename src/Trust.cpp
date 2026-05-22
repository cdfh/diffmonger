#include "Trust.hpp"
#include "Node.hpp"
#include "RepositoryStructure.hpp"

#include <cassert>

namespace diffmonger::trust {

std::optional<RepositoryStructure::Breadcrumb> maybeUpdateBreadcrumb(
    RepositoryStructure const &repositoryStructure,
    RepositoryStructure::Breadcrumb const &breadcrumb,
    BackendDriver::SnapshotId::Encoded const &candidate,
    BackendDriver::SnapshotId::timestamp_t::duration
    const min_time_between_snapshots,
    BackendDriver::SnapshotId::timestamp_t::clock::time_point const &now)
{
    if (breadcrumb.untrusted_begin == Node::initial_value())
        // The given candidate is the initial node; always trust it.
        return { RepositoryStructure::Breadcrumb
                 { .untrusted_begin = breadcrumb.untrusted_begin.next(),
                   .last_trusted_node_authorised_at = now } };

    if (candidate.node != breadcrumb.untrusted_begin)
        throw std::invalid_argument("is_trusted: invalid candidate");

    auto const cursnapshot = repositoryStructure.getEncodedSnapshotId(
        Node{breadcrumb.untrusted_begin.value - 1});

    // There is zero legitimate reason for the candidate to have an earlier timestamp.
    // A legitimate repository, upon recognising that it has generated a node earlier
    // than the end of the quarantine period, needs to voluntarily raise its timestamp
    // to the end of the quarantine.
    //
    // If a legitimate candidate had experienced non-monotonic clocks,
    // it would have detected that its now() time is before the previous node's
    // and voluntarily raised it to match. Thus, this is a sure indication of an attack;
    // never trust the node.
    // It's important that this decision is never changed; see below:
    // Suppose the candidate lies and sets its timestamp to N units in the past,
    // where N is so large that it dwarfs min_duration
    // (i.e., and thus this thought process assumes min_duration is 0).
    // Eventually the candidate will leave quarantine,
    // at which point it will become the trusted node.
    // But because it lied, it has now rewritten history and has bought the next
    // candidate a free N units.
    // If we wait alpha*N units before accepting the current candidate,
    // the the next candidate will have N + alpha*N = N*(1 + alpha) units as its claimed_duration.
    // It will be able to then distribute (1 + alpha)*N/min_duration nodes in that time
    // without further penalty. Had the candidates (current and future) been honest,
    // then in alpha*N units time it would have been able to process alpha*N/min_duration
    // nodes. (1 + alpha) > alpha, and so being dishonest created gain.
    // This cannot be tolerated.
    // Thus, we cannot wait alpha*N units before accepting the current candidate.
    // Instead, we wait N^2 units, after which the attacker can authorise
    // (N + N^2)/min_duration nodes. This is still greater than the N^2/min_duration
    // nodes that a legitimate repository could authorise in the same time,
    // so waiting N^2 doesn't work either...
    if (candidate.timestamp < cursnapshot.timestamp)
        return std::nullopt;

    // This is just to keep expected behaviour if the clock went back and
    // min_time_between_snapshots == 0 (i.e., expected behaviour being to
    // immediately trust all candidates).
    auto const effective_now = std::max(breadcrumb.last_trusted_node_authorised_at, now);

    auto const quarantine_ends =
        breadcrumb.last_trusted_node_authorised_at + min_time_between_snapshots;

    // Note: candidate.timestamp < quarantine_ends does not imply an attack; we allow it.
    auto const trusted_at = std::max(quarantine_ends, candidate.timestamp);
    if (effective_now >= trusted_at)
        return RepositoryStructure::Breadcrumb {
            .untrusted_begin = candidate.node.next(),
            .last_trusted_node_authorised_at = trusted_at
        };
    else
        return std::nullopt;
}

}
