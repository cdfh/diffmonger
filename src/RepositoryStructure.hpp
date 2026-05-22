#ifndef DIFFMONGER_REPOSITORY_STRUCTURE_HPP
#define DIFFMONGER_REPOSITORY_STRUCTURE_HPP

#include "Node.hpp"
#include "BackendDriver.hpp"

#include <diffmonger/util/FdOwner.hpp>
#include <diffmonger/util/Uuid.hpp>

#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <expected>
#include <functional>

namespace diffmonger {

class KeyPair;

class RepositoryStructure
{
public:
    /**
     * Hub for collecting state related to a snapshot prior to its permanent
     * inclusion in the repository as a diffmonger node.
     * Until an instance of this class is commited,
     * both the instance and the associated state are considered ephemeral and can
     * safely be discarded without risk of corrupting the repository or dataset.
     * This class exists to eliminate risk of interrupted commands corrupting the
     * repository. The act of commiting an instance is performed atomically
     * by renaming a single directory on the filesystem.
     *
     * Ideally, the process of populating a TemporarySnapshot would have no side-effects.
     * However, one side effect is unavoidable:
     * the diffmonger snapshot command creates a snapshot in the underlying dataset,
     * and so destruction of the corresponding TemporarySnapshot instance makes this
     * snapshot unreachable.
     * This is not a data correctness concern, but would create garbage in the dataset
     * if left un mitigated. Therefore, each diffmonger repository has an UUID
     * associated with it and the BackendDriver is expected
     * to identify its snapshots deterministically on the basis of the node identifier
     * and the repository UUID. Therefore, if one attempt to take a diffmonger snapshot
     * leaves an orphaned snapshot in the underlying dataset,
     * then the next attempt shall detect the previously orphaned snapshot
     * (by virtue of knowing its name) and shall
     * re-incorporate it into the repository. The naming scheme (and in particular,
     * each repository having a UUID) guarantees that the adopted snapshot shall not
     * be unrelated and just coincidentally named. The Controller implements this
     * orchestration logic.
     *
     * Important: Only one TemporarySnapshot instance can exist for a given
     * RepositoryStructure at a time. This uniqueness in checked at runtime.
     *
     * The TemporarySnapshot has an associated directory in the repository.
     */
    class TemporarySnapshot
    {
        friend class RepositoryStructure;
        struct Impl;
    public:
        TemporarySnapshot(std::shared_ptr<Impl>);

        TemporarySnapshot(TemporarySnapshot const &) noexcept = delete;
        TemporarySnapshot &operator=(TemporarySnapshot const &) noexcept = delete;

        TemporarySnapshot(TemporarySnapshot &&) noexcept = default;
        TemporarySnapshot &operator=(TemporarySnapshot &&) noexcept = default;

        FdOwner &getFd();

        /**
         * No cleanup logic in the destructor:
         * partial state is limited to the associated directory,
         * which will be cleaned if necessary upon the next construction of a
         * TemporarySnapshot.
         * Putting cleanup logic in the destructor would be brittle:
         * the "fault vector" is not limited to C++ exceptions,
         * but also power-loss of the underlying system.
         * Clearly, any cleanup logic would be useless in protecting against unexpected
         * power loss, hence, the cleanup belongs in the constructor and not the destructor.
         */
        ~TemporarySnapshot();
    private:
        void commit(RepositoryStructure const &repositoryStructure,
                    BackendDriver::SnapshotId::Encoded const &);
        std::shared_ptr<Impl> impl;
    };


    /**
     * Checks out the single TemporarySnapshot instance associated with this instance,
     * or throws if it has already been checked out and not yet returned by
     * commit().
     */
    TemporarySnapshot checkOutTemporarySnapshot();

    /**
     * Commit the TemporarySnapshot into the repository.
     * The final action of this is to rename the temporary directory associated with the
     * TemporarySnapshot from its temporary name to its permament name.
     * The only effects of this method are changes to the directory and its contents.
     * If the code throws or otherwise does not complete the rename operation,
     * then the temporary directory will be discarded by diffmonger next time a temporary
     * directory is needed by the current repository.
     * In this way, the current method is exception safe:
     * while the temporary directory may be left in a partial state upon an
     * exception being thrown, this partial state has no subsequent effect.
     */
    void commit(TemporarySnapshot temporarySnapshot,
                BackendDriver::SnapshotId::Encoded const &);

    std::filesystem::path getPayloadFile(Node const node) const;

    BackendDriver::SnapshotId::Encoded getEncodedSnapshotId(Node const node) const;

    std::optional<BackendDriver::SnapshotId::Encoded>
    tryGetEncodedSnapshotId(Node const node) const;

    /**
     * Returns a sorted vector of KeyPairs known to the repository.
     * Note that this should never be used for decryption operations.
     * This is because returning a vector is sensitive to DoS attacks where
     * ransomware spams the repository by creating spurious keypairs,
     * thereby preventing restore operations due to readKeyPairs() always throwing
     * std::bad_alloc. This doesn't matter for encryption operations because
     * encrypt operations are never attempts to restore.
     * For decryption related operatinos, see instead withKeyPairs().
     */
    std::vector<std::unique_ptr<KeyPair>> readKeyPairs() const;

    /**
     * DoS safe alternative to readKeyPairs().
     */
    void withKeyPairs(
        std::function<bool(KeyPair const &)> const &f) const;

    /**
     * Get the repository's initargs.
     * Guaranteed to be non-empty.
     */
    std::vector<std::string> const &getInitArgs() const;
    Uuid getRepositoryStructureFormatUuid() const;

    std::optional<BackendDriver::SnapshotId::Encoded>
    getNodeRelativeToLastTrusted(std::make_signed_t<Node::value_t> offset) const;

    /**
     * Get all nodes, including untrusted nodes, up to (not including)
     * the given end node, if non-zero.
     * If the given end node is zero, returns all nodes up to
     * getNextsnapshotNode()
     */
    std::vector<Node> getNodesIncludingUntrusted(Node end = Node{0}) const;

    /**
     * State regarding the most recently trusted node.
     * This state is needed for identifying the current node and for quarantining purposes.
     * It is stored in a mutable file and updated whenever the repository
     * is pruned (both when pruning was triggered directly by the prune-repository
     * command, and when pruning was triggered indirectly by taking a snapshot).
     * State that is written locally does not need to be mirrored to remote storage
     * locations; instead, each time the remote storage locations run the prune-repository
     * command, they discover new snapshots via fastForward() and then update their
     * own local state files.
     *
     * Notes: storage of this state is unfortunately necessary under diffmonger's
     * threat model, in particular, that an attacker can create arbitrary files in the
     * repository directory. If the threat model did not include this,
     * then a discovery algorithm could find the most recent node either in O(log(n)^2)
     * (by successive queries for node existance on the nodes of the tree),
     * or in O(log(n)) (by reading the snapshots dir).
     * However, both algorithms are vulnerable to an attacker creating
     * spurious snapshot files. The tree discovery method initially appears safe as it only
     * relies upon querying nodes that are known to have once existed,
     * and while a node exists,
     * the threat model assumes that an attacker cannot mutate its state.
     * However, it has a problem: once a node is pruned, the attacker is free to
     * create a new malignant node under the old identifier, misleading the algorithm.
     */
    struct Breadcrumb
    {
        Node untrusted_begin;
        /**
         * Time at which the last trusted node became trusted.
         */
        BackendDriver::SnapshotId::timestamp_t last_trusted_node_authorised_at;
    };
    Breadcrumb const &getBreadcrumb() const;

    /**
     * Write the given breadcrumb to the repository.
     * The following invariants must be true before the call:
     *   - the repository must be fully pruned with respect to the new node that will
     *     become trusted as a consequence of the call
     *     (i.e., as though by getBreadcrumb().untrusted_begin.prune()).
     */
    void writeBreadcrumb(Breadcrumb const &breadcrumb);

    /**
     * Get the latest trusted node.
     * The Controller must ensure that the repository always contains at least
     * the tree::fenwick_path() from the returned node.
     * Under normal circumstances, the repository shall also contain the full
     * tree::alive_after() set. However, in some circumstances
     * (in particular, if a previous attempt to take a snapshot failed after pruning
     * but before taking the snapshot), only a subset of this set may be present,
     * but this subset shall include at least the described tree::fenwick_path().
     */
    std::optional<Node> getLatestTrustedNode() const
    {
        auto const node = getBreadcrumb().untrusted_begin;
        return node == Node::initial_value() ? std::nullopt : std::optional{node};
    }

    /**
     * Returns the node of the next snapshot.
     * Note that snapshots implicitly trust all nodes,
     * and so this fast-forwards to the most recently taken node and then returns the
     * next consecutive value.
     */
    Node getNextSnapshotNode() const;

    bool nodeExists(Node const node) const;

    FdOwner createPayloadFdForReading(Node node) const;

    void deleteNode(Node node);

    /**
     * Get the repository UUID.
     * The only reason the repository has a UUID is to allow it to create snapshots
     * with the guarantee that an identically identified snapshot shall only exist
     * if the current repository already created the same snapshot.
     * The UUID should not be used for other purposes.
     * Note that it is possible for a repository to contain snapshots with a UUID
     * different to its own; this happens when import repositories.
     */
    Uuid getRepositoryUuid() const;

    // Conflicted about exposing this.
    // It should never be used to by-pass RepositoryStructure,
    // but it's useful for printing debugging messages, etc.
    std::filesystem::path const &getRepository() const;

    static void storeKeyPair(std::filesystem::path const &repository_path,
                             KeyPair const &keyPair);

    RepositoryStructure() = delete;
    RepositoryStructure(RepositoryStructure const &) = delete;
    RepositoryStructure(RepositoryStructure &&) = default;

    /**
     * Constructor.
     * Only a single RepositoryStructure instance should exist at any point in time
     * for a given repository.
     */
    RepositoryStructure(std::filesystem::path const &repository);

    RepositoryStructure &operator=(RepositoryStructure const &) = delete;
    RepositoryStructure &operator=(RepositoryStructure &&) = default;

    ~RepositoryStructure();

    static RepositoryStructure createRepository(std::filesystem::path const &repository_path,
                                                Uuid const &repositoryStructureFormatUuid,
                                                std::vector<std::string> const &initargs);

protected:
    std::filesystem::path pathForNode(Node const node) const;
    std::expected<BackendDriver::SnapshotId::Encoded, std::system_error>
    tryGetEncodedSnapshotId2(Node const node) const;
    template <typename F>
    void foreachNode(F &&f) const;
private:
    /**
     * Lockfile fd, upon which a F_OFD_SETLK lock is obtain
     * (locks for the duration of the fd being held).
     * Every RepositoryStructure instance obtains a lock, including read-only operations.
     * While there's no reason overlapping read-only operations couldn't be supported,
     * it would increase code complexity for the sake of supporting an exceedingly rare
     * use case.
     */
    FdOwner lockfd;
    std::filesystem::path repository;
    Uuid repositoryStructureFormatUuid;
    std::vector<std::string> initargs;
    Breadcrumb breadcrumb;
    std::shared_ptr<TemporarySnapshot::Impl> temporarySnapshotImpl;
};

}

#endif
