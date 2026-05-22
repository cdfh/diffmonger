#include "Controller.hpp"
#include "BackendDriver.hpp"
#include "EncryptionUtil.hpp"
#include "RepositoryParams.hpp"
#include "RepositoryStructure.hpp"
#include "KeyPair.hpp"
#include "sysutil.hpp"
#include "Trust.hpp"

#include <diffmonger/util/PidOwner.hpp>
#include <diffmonger/util/misc.hpp>
#include <diffmonger/util/Serialisation.hpp>
#include <diffmonger/util/ioutil.hpp>

#include <filesystem>
#include <functional>
#include <vector>
#include <set>
#include <iostream>
#include <span>
#include <tuple>
#include <array>
#include <iomanip>

#include <unistd.h>
#include <error.h>


namespace diffmonger {

namespace {

using Snapshot = BackendDriver::SnapshotId::Encoded;

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

    auto temporarySnapshot = repositoryStructure.checkOutTemporarySnapshot();

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

                encryptToFd(
                    repositoryStructure.readKeyPairs(),
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
void prune(RepositoryStructure const &repositoryStructure, F const fremove)
{
    auto const maybe_node = repositoryStructure.getLatestTrustedNode();

    if (!maybe_node)
        return;

    maybe_node->prune([&] (Node const node) { fremove(node); });
}

std::unique_ptr<DecryptedKeyPair>
decryptKeyPair(RepositoryStructure const &repositoryStructure,
               PasswordBuffer const &password)
{
    std::unique_ptr<DecryptedKeyPair> out;
    size_t slot = 0;
    repositoryStructure.withKeyPairs(
        [&] (KeyPair const &keyPair)
        {
            try
            {
                out = std::make_unique<DecryptedKeyPair>(keyPair, password);
            } catch (std::runtime_error const &e)
            {
                // No need to differentiate wrong password from other errors:
                // in either case, the correct course of action is to continue to try
                // other slots.
                std::cerr <<
                    "When decrypting slot " << slot << ": " << e.what() << "\n";
            }
            ++slot;

            return true;
        });
    return out;
}

} // <-- anonymous namespace

void init_repository(std::filesystem::path const &repository_path,
                     Uuid const &repositoryStructureFormatUuid,
                     std::vector<std::string> const &initargs)
{
    auto repositoryStructure =
        RepositoryStructure::createRepository(repository_path, repositoryStructureFormatUuid, initargs);
}

// TODO: Need to check whether exceeding maxKeySlots.
void init_keypair(std::filesystem::path const &repositoryPath,
                  InitKeyPairParams const &initKeyPairParams)
{
    KdfParams params{};

    params.memlimit = initKeyPairParams.memlimit;

    auto const decryptedKeyPair =
        DecryptedKeyPair::generateFromPassphrase(params,
                                                 *initKeyPairParams.password);
    RepositoryStructure::storeKeyPair(repositoryPath, decryptedKeyPair.getKeyPair());
}

void RepositoryParams::verify() const
{
}

namespace {
bool _snapshot(RepositoryStructure &repositoryStructure,
               RepositoryParams const &repositoryParams,
               BackendDriver &backendDriver,
               SnapshotParams const &)
{
    // Regarding not checking for snapshots that exceed the min_seconds_between_snapshots
    // rule:
    //   This function only adds snapshots, it does not itself prune snapshots.
    //   Adding snapshots is safe, so no need to consider invalids when adding snapshots.

    Node const node = repositoryStructure.getNextSnapshotNode();

    // Pruning.
    // Pruning intentionally occurs before taking the snapshot.
    // The comments in tree::prune() prove that the nodes that get pruned shall
    // never be in the fenwick_path() of the given node, and thus,
    // pruning can safely occur prior to storing the snapshot.
    prune(repositoryStructure,
          [&] (Node const node) // Node may have already been pruned; this is okay.
          {
              std::cout << "Pruning " << node.value << "\n";
              try
              {
                  // Does not throw if non-existing.
                  repositoryStructure.deleteNode(node);

                  // Throws SnapshotDoesNotExist if non-existing.
                  if (auto const maybeSnapshotId =
                      repositoryStructure.tryGetEncodedSnapshotId(node))
                      backendDriver.remove_snapshot(
                          *backendDriver.snapshotId(*maybeSnapshotId));
              }
              // If the snapshot does not exist in the backend,
              // then nodeToSnapshotId() will throw.
              // However, we don't care if it doesn't exist, as we're trying to delete
              // it anyway!
              catch (BackendDriver::SnapshotDoesNotExist const &) {}
          });


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


// Counterpart to export_repository(). The two commands work together.
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

    // Note: no need to prune after calling store_snapshot():
    // the exporting command will have only exported alive snapshots.
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
    std::unique_ptr<DecryptedKeyPair> const decryptedKeyPair =
        repositoryParams.encryption
        ? decryptKeyPair(
            repositoryStructure,
            params.password
            ? *params.password
            : throw std::runtime_error("Password needed but not given"))
        : std::unique_ptr<DecryptedKeyPair>{};

    auto const decrypted =
        [&] (Node const node, auto const &f)
        {
            auto input = repositoryStructure.createPayloadFdForReading(node);

            if (decryptedKeyPair)
            {
                fprintf(stderr, "encryption enabled for repository %s\n",
                        repositoryStructure.getRepository().c_str());
                withDecryptedInput(std::move(input), *decryptedKeyPair, f);
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
        params.node.fenwick_path_inc_root([&] (Node const node) { nodes.emplace(node); });
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
        for (Node const node: repositoryStructure.getNodesIncludingUntrusted())
        {
            if (node > params.node)
                break;
            if (nodes.contains(node.fenwick_parent())
                || (node == Node::initial_value()))
                nodes.emplace(node);
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


void prune_repository(RepositoryStructure &repositoryStructure,
                      RepositoryParams const &repositoryParams,
                      PruneParams const &)
{
    using timestamp_t = BackendDriver::SnapshotId::timestamp_t;
    auto const now = timestamp_t::clock::now();
    /*
     * Note: the following loop does pruning,
     * then calls RepositoryStructure::writeBreadcrumb().
     * Tempting as it may be, writeBreadcrumb() must not be refactored out of the loop
     * (i.e., so as to be called only once upon the loop's completion).
     * This is because the repository invariants must be maintained on the filesystem
     * so that, if at any point the process is spontaneously terminated,
     * the repository on-filesystem is in a valid state.
     */
    while (1)
    {
        auto const &breadcrumb = repositoryStructure.getBreadcrumb();
        auto const maybe_snapshot =
            repositoryStructure.tryGetEncodedSnapshotId(breadcrumb.untrusted_begin);

        if (!maybe_snapshot)
            break;

        auto const maybe_updated_breadcrumb =
            trust::maybeUpdateBreadcrumb(
                repositoryStructure,
                breadcrumb,
                *maybe_snapshot,
                std::chrono::duration_cast<timestamp_t::duration>(
                    std::chrono::seconds{repositoryParams.min_seconds_between_snapshots}),
                now);
        if (!maybe_updated_breadcrumb)
            break;
        breadcrumb.untrusted_begin.prune(
            [&] (Node const node)
            {
                repositoryStructure.deleteNode(node);
            });
        repositoryStructure.writeBreadcrumb(*maybe_updated_breadcrumb);
    }
}

ExitStatus export_repository(RepositoryStructure &repositoryStructure,
                             RepositoryParams const &repositoryParams,
                             BackendDriver &backendDriver,
                             ExportParams const &exportParams)
{
    Node node;

    if (Node const next = repositoryStructure.getNextSnapshotNode();
        next == Node::initial_value())
        // Nothing to do.
        return ExitStatus::SUCCESS;
    else
        node = {next.value - 1};

    RestoreParams const restoreParams {
        .password = exportParams.password,
        .node = node,
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
               ShowInitParams const &showInitParams)
{
    auto const initargs = repositoryStructure.getInitArgs();

    if (showInitParams.nullsep)
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

    auto const nodes = repositoryStructure.getNodesIncludingUntrusted();

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
