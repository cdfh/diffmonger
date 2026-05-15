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
     * it is considered ephemeral and can safely be discarded without risk of
     * corrupting the repository or dataset. However, a side effect of the
     * diffmonger snapshot command is that a snapshot is created in the underlying dataset,
     * and so destruction of the corresponding TemporarySnapshot instance makes this
     * snapshot unreachable, effectively orphaning it in the dataset.
     * This is not a data correctness concern
     * (creating snapshots in the dataset should never cause data correctness issues),
     * but does create garbage in the dataset.
     * In effort to mitigate accumulation of such garbage,
     * the repository has a UUID associated with it and the BackendDriver is expected
     * to identify its snapshots deterministically on the basis of the node identifier
     * and the repository UUID. Therefore, if one attempt to take a diffmonger snapshot
     * leaves an orphaned snapshot in the underlying dataset,
     * then the next attempt should detect the previously orphaned snapshot and
     * re-incorporate it into the repository. The Controller implements the
     * orchestration logic.
     *
     * Important: Only one TemporarySnapshot instance can exist for a given repository
     * at a time. This uniqueness is sadly not checked.
     *
     * The TemporarySnapshot has an associated directory in the repository.
     */
    class TemporarySnapshot
    {
        friend class RepositoryStructure;
    public:
        TemporarySnapshot(TemporarySnapshot &&);
        TemporarySnapshot &operator=(TemporarySnapshot &&);

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

    protected:
        TemporarySnapshot(std::filesystem::path const &repository);
        void commit(std::filesystem::path const &rename_to,
                    BackendDriver::SnapshotId::Encoded const &);
    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

#if 0
    /**
     * Convenience class as initargs annoyingly needs to be converted
     * to vector<string_view> for parser.
     */
    class InitArgsPair
    {
    public:
        std::vector<std::string> const &getInitArgs();
        Uuid repositoryStructureFormatUuid;
        std::vector<std::string> const initargs;
        std::vector<std::string_view> const initargs_stringview =
            std::vector<std::string_view>(initargs.begin(), initargs.end());

        std::span<std::string_view const> span_exc_front() const
        {
            return { std::next(initargs_stringview.begin()),
                     initargs_stringview.end() };
        }

        std::string const &getName() const
        {
            return initargs.front();
        }

        InitArgsPair(Uuid const &repositoryStructureFormatUuid,
                     std::vector<std::string> initargs)
            : repositoryStructureFormatUuid{repositoryStructureFormatUuid},
              initargs{
                  initargs.empty()
                  ? throw std::runtime_error("Initargs invalid; corrupted repository?")
                  : initargs}

        {}
    };
#endif


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
        std::function<bool(KeyPair const &, size_t)> const &f) const;

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

    /**
     * Get the repository's initargs.
     * Guaranteed to be non-empty.
     */
    std::vector<std::string> const &getInitArgs() const;
    Uuid getRepositoryStructureFormatUuid() const;

    std::vector<Node> getNodes() const;
    std::optional<Node> getLatestNode() const;

    bool nodeExists(Node const node) const;

    FdOwner createPayloadFdForReading(Node node) const;

    TemporarySnapshot createTemporarySnapshot();

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

    static void storeKeyPair(std::filesystem::path repository_path,
                             KeyPair const &keyPair);

    RepositoryStructure() = delete;
    RepositoryStructure(RepositoryStructure const &);
    RepositoryStructure(std::filesystem::path const &repository);

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
    std::filesystem::path repository;
    Uuid repositoryStructureFormatUuid;
    std::vector<std::string> initargs;
};

}

#endif
