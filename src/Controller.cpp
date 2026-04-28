#include "Controller.hpp"
#include "BackendDriver.hpp"
#include "EncryptionUtil.hpp"
#include "diffmonger.hpp"
#include "RepositoryParams.hpp"
#include "RepositoryStructure.hpp"
#include "KeyPair.hpp"
#include "sysutil.hpp"

#include <diffmonger/util/PidOwner.hpp>
#include <diffmonger/util/misc.hpp>
#include <diffmonger/util/Serialisation.hpp>
#include <diffmonger/util/ioutil.hpp>

#include <filesystem>
#include <functional>
#include <vector>
#include <map>
#include <set>
#include <iterator>
#include <iostream>
#include <algorithm>
#include <span>
#include <tuple>
#include <array>
#include <iomanip>

#include <unistd.h>
#include <error.h>


namespace diffmonger {

namespace {

using Snapshot = BackendDriver::SnapshotId::Encoded;

/**
 * Given a span of snapshots, sorted by \c Snapshot::node,
 * returns an iterator that points to the first spurious element of the span,
 * where a spurious element is an element that breaks the min-time-between-snapshots rule.
 */
std::span<Snapshot const>::iterator apply_time_constraints(
    std::span<Snapshot const> const snapshots,
    BackendDriver::SnapshotId::timestamp_t::duration
    const min_time_between_snapshots)
{
    auto const now = BackendDriver::SnapshotId::timestamp_t::clock::now();

    auto it = snapshots.begin();
    auto end = snapshots.end();

    if (it == end)
        return it;

    auto effective_timestamp = it->timestamp;
    auto quarantine_begins_it = std::next(it);

    for (++it; it != end; ++it)
    {
        // Removing this early exit would not affect correctness.
        if (effective_timestamp > now)
            return quarantine_begins_it;

        if (it->timestamp < std::prev(it)->timestamp)
            // Danger: the current timestamp is _earlier_ than the previous;
            // without care, this would circumvent the protections we seek to add.
            // Once the current node becomes matures out of quarantine,
            // the previous node(s) will at some point get garbage collected,
            // at which point we will no longer be able to tell that the current node
            // lied about its timestamp.
            // Thus, we need to extend the quarantine
            // period for long enough that such a history re-write is not dangerous.
            // To do this, we add a penalty equal to the duration that was lied about.
            effective_timestamp += std::prev(it)->timestamp - it->timestamp;
        else
        {
            // Timestamps from the future are not a threat and are falsely
            // penalising themselves. They are likely due to mismatching clocks.
            // To avoid needless cumulation of debt for such non-threats,
            // cap the time to now.
            auto const timestamp = std::min(it->timestamp, now);
            auto const minimum = effective_timestamp + min_time_between_snapshots;

            if (timestamp < minimum)
                effective_timestamp = minimum;
            else
            {
                effective_timestamp = timestamp;

                if (effective_timestamp <= now)
                {
                    // std::prev(it) is safe. It's in the past and it didn't affect
                    // the current effective_timestamp value. Remove it from quarantine.
                    quarantine_begins_it = it;
                }
            }
        }
    }

    if (effective_timestamp <= now)
        quarantine_begins_it = end;

    return quarantine_begins_it;
}

std::pair<std::span<Snapshot const>, std::span<Snapshot const>> apply_time_constraints2(
    std::span<Snapshot const> const snapshots,
    BackendDriver::SnapshotId::timestamp_t::duration
    const min_time_between_snapshots)
{
    auto const it = apply_time_constraints(snapshots, min_time_between_snapshots);
    return std::make_pair(std::span(snapshots.begin(), it), std::span(it, snapshots.end()));
}

/**
 * Read snapshots from the repository, but do not apply time constraints
 * (see apply_time_constraints()).
 * Snapshots are sorted by order of increasing node.
 */
std::vector<Snapshot> readSnapshots(RepositoryStructure const &repositoryStructure)
{
    auto nodes = repositoryStructure.getNodes();

    std::sort(nodes.begin(), nodes.end());

    std::vector<Snapshot> out;

    for (auto const &node: nodes)
        out.push_back(repositoryStructure.getEncodedSnapshotId(node));

    return out;
}

struct MissingNode : std::runtime_error
{
    MissingNode(Node const node)
        : runtime_error("Missing node: " + std::to_string(node.value)),
          node(node)
    {}

    Node const node;
};

void store_snapshot(RepositoryStructure &repositoryStructure,
                    BackendDriver::SnapshotId const &snapshotId,
                    BackendDriver::SnapshotId const *maybeParentSnapshotId,
                    RepositoryParams const &repositoryParams,
                    BackendDriver &backendDriver)
{
    Node const node = snapshotId.getNode();

    auto temporarySnapshot = repositoryStructure.createTemporarySnapshot();

    auto const storeSnapshotOrDiff =
        [&] (FdOwner out)
        {
            if (node == Node::initial_value())
                backendDriver.initial(snapshotId, std::move(out));
            else
            {
                if (!maybeParentSnapshotId)
                    throw std::logic_error(
                        "Bug: store_snapshot() not given parent when needed");
                backendDriver.diff(*maybeParentSnapshotId,
                                   snapshotId,
                                   std::move(out));
            }
        };

    if (repositoryParams.encryption)
    {
        auto driver_to_encryption_pair = FdPipePair::create(O_CLOEXEC);

        // Important: this needs to be done /before/ backendDriver.initial()
        // or backendDriver.diff().
        // This is because both backendDriver methods block until the backend process
        // finishes,
        // and so if we've not already set up a consuming end on the pipe,
        // the backendDriver will end up blocking indefinitely once the pipe becomes full.
        auto encryptionPid = PidOwner::runInChild(
            [&]
            {
                // Important: must close() the write_end otherwise EOF will never
                // be sent to the reader.
                driver_to_encryption_pair.write_end.reset();

                encryptToFd(*repositoryStructure.readKeyPair(),
                            std::move(driver_to_encryption_pair.read_end),
                            std::move(temporarySnapshot.getFd()));
            });

        // Important: if this isn't close()ed, then storeSnapshotOrDiff hangs if the child
        // throws an exception; this is because the read_end won't send EOF if
        // it is still open in the parent. We rely upon the process exec()ed by
        // storeSnapshotOrDiff being killed by SIGPIPE if the above child dies.
        driver_to_encryption_pair.read_end.reset();
        temporarySnapshot.getFd().reset();

        storeSnapshotOrDiff(std::move(driver_to_encryption_pair.write_end));

        // Don't rely on the PidOwner destructor, it can't throw!
        encryptionPid.wait_or_throw();
    } else
    {
        storeSnapshotOrDiff(std::move(temporarySnapshot.getFd()));
    }

    repositoryStructure.commit(std::move(temporarySnapshot),
                               snapshotId.encoded());
}

std::unique_ptr<BackendDriver::SnapshotId>
nodeToSnapshotId(RepositoryStructure const &repositoryStructure,
                 BackendDriver &backendDriver,
                 Node const node)
{
    return backendDriver.snapshotId(repositoryStructure.getEncodedSnapshotId(node));
}

template <typename F>
void prune(RepositoryStructure const &repositoryStructure,
           RepositoryParams const &repositoryParams,
           std::optional<Node> node,
           F const fremove)
{
    auto const all_snapshots = readSnapshots(repositoryStructure);

    auto const dopruning =
        [&] (Node const node, std::span<Snapshot const> const snapshots)
        {
            if (snapshots.empty())
                return;

            if (node > snapshots.back().node)
                throw std::runtime_error("Requested to prune with respect to a node that is "
                                         "more recent than any stored snapshots!");

            std::set<Node> aliveset;
            node.alive_after([&aliveset] (Node const node) { aliveset.emplace(node); });

            for (auto const &snapshot: snapshots)
                if (!aliveset.contains(snapshot.node) && (snapshot.node <= node))
                    fremove(snapshot.node);
        };

    if (node.has_value())
        dopruning(node.value(), all_snapshots);
    else
    {
        using timestamp_t = BackendDriver::SnapshotId::timestamp_t;
        auto const valid_snapshots =
            apply_time_constraints2(
                all_snapshots,
                std::chrono::duration_cast<timestamp_t::duration>(
                    std::chrono::seconds{
                        repositoryParams.min_seconds_between_snapshots})).second;

        if (valid_snapshots.empty())
            return;

        dopruning(valid_snapshots.back().node, valid_snapshots);
    }
}

} // <-- anonymous namespace

void init_repository(std::filesystem::path const &repository_path,
                     std::vector<std::string> const &initargs)
{
    auto repositoryStructure =
        RepositoryStructure::createRepository(repository_path, initargs);
}

void init_keypair(std::filesystem::path const &repositoryPath,
                  InitKeyPairParams const &initKeyPairParams)
{
    auto decryptedKeyPair = DecryptedKeyPair::generateFromPassphrase(*initKeyPairParams.password);
    RepositoryStructure::storeKeys(repositoryPath, decryptedKeyPair.getKeyPair());
}

void RepositoryParams::verify() const
{
}

namespace {
bool _snapshot(RepositoryStructure &repositoryStructure,
               RepositoryParams const &repositoryParams,
               BackendDriver &backendDriver,
               SnapshotParams const &params)
{
    // Regarding not checking for snapshots that exceed the min_seconds_between_snapshots
    // rule:
    //   This function only adds snapshots, it does not itself prune snapshots.
    //   Adding snapshots is safe, so no need to consider invalids when adding snapshots.
    //   While this function does call prune_filesystem() and prune_repository(),
    //   those commands do their own checks.

    Node const node = repositoryStructure.getLatestNode()
        .transform([] (auto const node) { return node.next(); })
        .value_or(Node{0});

    auto const maybeParentSnapshotId =
        node.fenwick_maybe_parent()
        .and_then(
            [&] (Node const parent)
            {
                auto parentSnapshotId =
                    nodeToSnapshotId(repositoryStructure, backendDriver, parent);
                if (!backendDriver.exists(*parentSnapshotId))
                    throw
                        BackendDriver::SnapshotDoesNotExist(
                            *parentSnapshotId,
                            std::string("Parent snapshot (")
                            .append(parentSnapshotId->pretty())
                            .append(") does not exist; "
                                    "it must be created by restoring to the dataset."));
                return std::optional{std::move(parentSnapshotId)}; })
        .value_or(nullptr);

    auto const [snapshot, already_old] =
        backendDriver.snapshot(node, repositoryStructure.getRepositoryUuid());

    store_snapshot(
        repositoryStructure,
        *snapshot,
        maybeParentSnapshotId.get(),
        repositoryParams,
        backendDriver);

    PruneDatasetParams const pruneDatasetParams { .node = node };
    PruneParams const pruneParams { .node = node };

    if (params.prune_dataset)
        prune_dataset(repositoryStructure,
                      repositoryParams,
                      backendDriver,
                      pruneDatasetParams);
    if (params.prune_repository)
        prune_repository(repositoryStructure, repositoryParams, pruneParams);

    return !already_old;
}}


void snapshot(RepositoryStructure &repositoryStructure,
              RepositoryParams const &repositoryParams,
              BackendDriver &backendDriver,
              SnapshotParams const &params)
{
    while (!_snapshot(repositoryStructure,
                      repositoryParams,
                      backendDriver,
                      params))
        printf("%s\n", "Snapshot was old");
}

void snapshot_import(RepositoryStructure &repositoryStructure,
                     RepositoryParams const &repositoryParams,
                     BackendDriver &backendDriver,
                     SnapshotImportParams const &params)
{
    // Protocol between snapshot_import() and export_repository().
    auto const stdinbytes = ioutil::read2(FdOwner::dup(STDIN_FILENO));

    auto const encoded = BackendDriver::SnapshotId::Encoded::deserialise(stdinbytes);

    /*
     * TOOD: In the long run, it would be nice to be able to continue from
     * a previous export. However, there are several complexities:
     *   1. On the export side, restore() needs to be told it doesn't need to
     *      restore each node (which is expensive).
     *   2. If the stored snapshot was created (i.e., due to params.create_snapshot),
     *      then we need to be sure the stored snapshot truely corresponds to the
     *      snapshot currently being restored.
     *      This is probably best done by adding a member to Encoded
     *      (which needs to be interpretable by the Controller, not just the BackendDriver)
     *      that stores the origin SnapshotId, if one exists.
     *      An origin exists if and only if a snapshot is created
     *      (i.e., with BackendDriver::snapshot()) by snapshot_import()
     *      Note that if the stored snapshot was adaptod by snapshot_import()
     *      (rather than being created),
     *      then everything is easy, as the binary encoded representation
     *      does not change after adoption, so it's trivial to verify the already
     *      stored snapshot corresponds to the currently restored snapshot.
     */
    if (repositoryStructure.nodeExists(encoded.node))
        throw std::runtime_error("Cannot import snapshot: node already exists");

    auto snapshotId =
        params.create_snapshot
        // Node created by /creating/ a new snapshot.
        ? backendDriver.snapshot(encoded.node,
                                 repositoryStructure.getRepositoryUuid()).first
        // Node created by /adopting/ the existing snapshot.
        : backendDriver.snapshotId(encoded);

    auto const maybeParentSnapshotId =
        encoded.node.fenwick_maybe_parent()
        .transform(
            [&] (Node const parent)
            {
                return nodeToSnapshotId(repositoryStructure, backendDriver, parent);
            })
        .value_or(nullptr);

    store_snapshot(
        repositoryStructure,
        *snapshotId,
        maybeParentSnapshotId.get(),
        repositoryParams,
        backendDriver);
}

namespace {
template <typename F>
void withDecryptedInput(FdOwner input,
                        DecryptedKeyPair const &decryptedKeyPair,
                        F &&f)
{
    // Note: No close-on-exec.
    auto decryption_to_driver_pair = FdPipePair::create(0);

    auto decryptionPid = PidOwner::runInChild(
        [&]
        {
            // Important: must close() the unused end (read_end)!
            decryption_to_driver_pair.read_end.reset();

            decryptFromFd(decryptedKeyPair,
                          std::move(input),
                          std::move(decryption_to_driver_pair.write_end));
        });

    input.reset();
    decryption_to_driver_pair.write_end.reset();

    f(std::move(decryption_to_driver_pair.read_end));

    decryptionPid.wait_or_throw();
}
}


void restore(RepositoryStructure const &repositoryStructure,
             RepositoryParams const &repositoryParams,
             BackendDriver &backendDriver,
             RestoreParams const &params,
             bool const restore_all_available_nodes,
             std::function<void(std::unique_ptr<BackendDriver::SnapshotId>)>
             const &uponEachSnapshot = {})
{
    std::optional<DecryptedKeyPair> const decryptedKeyPair =
        repositoryParams.encryption
        ? repositoryStructure.readKeyPair()->decrypt(*params.password)
        : std::optional<DecryptedKeyPair>{};

    auto const decrypted =
        [&] (Node const node, auto const &f)
        {
            auto input = repositoryStructure.createPayloadFdForReading(node);

            if (decryptedKeyPair)
            {
                fprintf(stderr, "encryption enabled for repository %s\n",
                        repositoryStructure.getRepository().c_str());
                withDecryptedInput(std::move(input), decryptedKeyPair.value(), f);
            } else
            {
                fprintf(stderr, "no encryption enabled\n");
                f(std::move(input));
            }
        };

    // Invariant: Shall always be self contained
    // (i.e., set contains every node's fenwick parent, up to the root).
    std::set<Node> nodes;

    if (!restore_all_available_nodes)
    {
        auto const node = params.node.or_else(
            [&] { return repositoryStructure.getLatestNode(); });

        if (node)
            fenwick_path_inc_root(node->value,
                                  [&] (size_t const node)
                                  { nodes.emplace(node); });
    } else
    {
        // Do not do the following:
        //   alive_after(node.value, [&] (size_t const node) { nodes.emplace_back(node); });
        // Because if node is historic, then even though the Fenwick path for the given
        // node may exist in the repository, the diffmonger nodes that would have been
        // alive when the given node was current (i.e., as given by alive_after(node, ...))
        // may no longer be alive (e.g., for zfs, it's impossible to restore a tree
        // snapshot structure; fundamentally, only linear histories can be reconstructed,
        // and so only the Fenwick parents are guranateed to exist).
        // The act of least surprise is to restore all snapshots that are available,
        // up to and including the given node. If fewer are desired, the user should specify
        // required_nodes_only.

        // readSnapshots(): output sorted by node.
        for (auto const &snapshot: readSnapshots(repositoryStructure))
        {
            if (params.node && (snapshot.node > *params.node))
                break;
            if (nodes.contains(snapshot.node.fenwick_parent())
                || (snapshot.node == Node::initial_value()))
                nodes.emplace(snapshot.node);
        }
    }

    for (auto const node: nodes)
    {
        auto snapshotId = nodeToSnapshotId(repositoryStructure, backendDriver, node);
        std::cerr << "restoring snapshot: " + snapshotId->pretty_diagnostics() << std::endl;
        if (!backendDriver.exists(*snapshotId) || !params.skip_existing)
        {
            decrypted(
                node,
                [&] (FdOwner in)
                {
                    if (node == Node::initial_value())
                        backendDriver.restoreInitial(
                            *snapshotId,
                            std::move(in));
                    else
                        backendDriver.restoreDiff(
                            *nodeToSnapshotId(repositoryStructure, backendDriver,
                                              node.fenwick_parent()),
                            *snapshotId,
                            std::move(in));

                });
        } else std::cerr << "(skipped)\n";

        if (uponEachSnapshot)
            uponEachSnapshot(std::move(snapshotId));
    }
}

void restore(RepositoryStructure &repositoryStructure,
             RepositoryParams const &repositoryParams,
             BackendDriver &backendDriver,
             RestoreParams const &params)
{
    restore(repositoryStructure, repositoryParams, backendDriver, params, false);
}

// TODO: Add a dry-run option.
void prune_dataset(RepositoryStructure &repositoryStructure,
                   RepositoryParams const &repositoryParams,
                   BackendDriver &backendDriver,
                   PruneDatasetParams const &pruneDatasetParams)
{
    prune(repositoryStructure,
          repositoryParams,
          pruneDatasetParams.node,
          [&] (Node const node)
          {
              try
              {
                  backendDriver.remove_snapshot(
                      *nodeToSnapshotId(repositoryStructure, backendDriver, node));
              }
              // If the snapshot does not exist in the backend,
              // then nodeToSnapshotId() will throw.
              // However, we don't care if it doesn't exist, as we're trying to delete
              // it anyway!
              catch (BackendDriver::SnapshotDoesNotExist const &) {}
          });
}

void prune_repository(RepositoryStructure &repositoryStructure,
                      RepositoryParams const &repositoryParams,
                      PruneParams const &pruneParams)
{
    prune(repositoryStructure,
          repositoryParams,
          pruneParams.node,
          [&] (Node const node)
          {
              std::cout << "Pruning " << node.value << "\n";

              repositoryStructure.deleteNode(node);
          });
}

ExitStatus export_repository(RepositoryStructure &repositoryStructure,
                             RepositoryParams const &repositoryParams,
                             BackendDriver &backendDriver,
                             ExportParams const &exportParams)
{
    RestoreParams const restoreParams {
        .password = exportParams.password,
        .node = std::nullopt,
        .skip_existing = false
    };

    if (exportParams.importCommand.empty())
        throw std::runtime_error("Invalid command: empty command");

    restore(
        repositoryStructure,
        repositoryParams,
        backendDriver,
        restoreParams,
        true,
        [&] (std::unique_ptr<BackendDriver::SnapshotId> snapshotId)
        {
            // O_CLOEXEC is important: the child can't have access to the write end
            // (or else it will never receive EOF when reading from the read end).
            auto pipePair = FdPipePair::create(O_CLOEXEC);

            with_null_terminated_array_of_cstrs2(
                exportParams.importCommand,
                [&] (char const * const * const argv)
                {
                    PidOwner child = systemvp3(exportParams.importCommand.front().c_str(),
                                               argv,
                                               std::move(pipePair.read_end));

                    auto const serialised = snapshotId->encoded().serialise();

                    // Protocol between export_repository() and snapshot_import().
                    ioutil::write2(std::move(pipePair.write_end), serialised);

                    try
                    {
                        child.wait_or_throw();
                    } catch (std::runtime_error const &e)
                    {
                        throw std::runtime_error(
                            "Snapshot command failed: " + std::string(e.what()));
                    }
                });
        });

    // Possibly error exit statuses should reflect whether the destination
    // repository got touched.
    return ExitStatus::SUCCESS;
}

void show_init(RepositoryStructure const &repositoryStructure,
               InitCmdParams const &initCmdParams)
{
    auto const initargs = repositoryStructure.getInitArgs();

    if (initCmdParams.nullsep)
    {
        for (auto const &initarg: initargs)
            printf("%s%c", initarg.c_str(), '\0');
    } else
    {
        auto pid = PidOwner::runInChild(
            [&]
            {
                std::vector<char const *> args = {
                    "bash",
                    "-c",
                    "printf '%q' \"$0\"; "
                    "printf ' %q' \"$@\"; "
                    "printf '\n'" };
                for (auto const &initarg: initargs)
                    args.push_back(initarg.c_str());
                args.push_back(nullptr);

                execvp(args[0], const_cast<char **>(args.data()));
                ::error(EXIT_FAILURE, errno, "Could not run bush");
            });

        pid.wait_or_throw();
    }
}

void list_nodes(RepositoryStructure const &repositoryStructure,
                RepositoryParams const &repositoryParams,
                ListNodesParams const &listNodesParams)
{
    (void) repositoryParams;

    auto const nodes = repositoryStructure.getNodes();

    for (const auto &node : nodes)
    {
        std::cout << node.value;

        if (listNodesParams.show_timestamps)
        {
            auto const encoded = repositoryStructure.getEncodedSnapshotId(node);

            std::time_t const tt =
                BackendDriver::SnapshotId::timestamp_t::clock::to_time_t(encoded.timestamp);

            std::tm tm = *std::gmtime(&tt);

            std::cout << " ";
            std::cout << std::put_time(&tm, "-%Y-%m-%d-%Hh%Mm-UTC");
        }

        std::cout << "\n";
    }

}

}
