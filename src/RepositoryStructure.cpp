#include "RepositoryStructure.hpp"
#include "KeyPair.hpp"

#include "cstdio-util.hpp"

#include <diffmonger/util/Serialisation.hpp>
#include <diffmonger/util/array.hpp>
#include <diffmonger/util/misc.hpp>
#include <diffmonger/util/ioutil.hpp>

#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <cstdio>
#include <algorithm>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>

namespace diffmonger {

namespace {
inline std::filesystem::path snapshotsDirName() { return "snapshots"; }
inline std::filesystem::path temporarySnapshotDirName() { return "temporary-snapshot"; }
inline std::filesystem::path payloadFileName() { return "blob"; }
inline std::filesystem::path idFileName() { return "id"; }
inline std::filesystem::path initArgsFileName() { return "initargs"; }
inline std::filesystem::path keysDirName() { return "keys"; }
inline std::filesystem::path userDataDirName() { return "user-data"; }
inline std::filesystem::path breadcrumbFileName() { return "breadcrumb"; }
inline std::filesystem::path lockFileName() { return "lock"; }
inline std::filesystem::path uuidFileName() { return "repository-uuid"; }

static constexpr size_t maxKeySlots = 255;


/*
 * Iterates over each keypair.
 * For simplicity, it is not possible to delete keypairs.
 * If this functionality is deemed necessary in future, the best approach will be to
 * empty the file without deleting it, or to overwrite the contents with a new keypair
 * that is immediately forgotten. Recall that a well configured
 * remote mirror will never reproduce
 * modifications or deletions, so deleting a keypair will not prevent existing data
 * from being decryptable with the keypair in question on the remote repository.
 */
template <typename F>
void withKeyFiles(std::filesystem::path const &repository, F &&f)
{
    auto const dir = repository/keysDirName();

    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
        throw std::runtime_error("Invalid keys directory for repository");

    /*
     * We cannot do a regular traversal of the directory because an attacker
     * may have made the directory infinitely populated in effort to hinder recovery.
     */
    for (size_t i = 0; i != maxKeySlots; ++i)
    {
        auto const filename = dir/std::to_string(i);
        if (std::filesystem::exists(filename))
        {
            if (f(filename))
                return;
        } else
            return;
    }
}


std::vector<std::filesystem::path>
getKeyFiles(std::filesystem::path const &repository)
{
    std::vector<std::filesystem::path> out;

    withKeyFiles(
        repository,
        [&] (std::filesystem::path const &path)
        {
            out.push_back(path);
            return false;
        });

    return out;
}

}

struct RepositoryStructure::TemporarySnapshot::Impl
{
    std::filesystem::path repository;
    FdOwner fd;
    std::optional<BackendDriver::SnapshotId::Encoded> encoded;

    Impl(RepositoryStructure const &repositoryStructure)
        : repository(repositoryStructure.getRepository())
    {
        auto const path = getPath();
        auto const payloadFile = path/payloadFileName();

        if (std::filesystem::exists(path))
        {
            std::filesystem::remove(path/idFileName());
            std::filesystem::remove(payloadFile);
        } else
            std::filesystem::create_directories(path);

        fd = FdOwner::from_syscall(open(payloadFile.c_str(),
                                        O_CREAT|O_WRONLY|O_CLOEXEC|O_EXCL,
                                        S_IRUSR|S_IWUSR),
                                   "Could not open file",
                                   payloadFile.native());
    }

    std::filesystem::path getPayloadFile() const { return getPath()/payloadFileName(); }
    std::filesystem::path getIdFile() const { return getPath()/idFileName(); }
    std::filesystem::path getPath() const { return repository/temporarySnapshotDirName(); }
};

FdOwner &RepositoryStructure::TemporarySnapshot::getFd() { return impl->fd; }

RepositoryStructure::TemporarySnapshot::TemporarySnapshot(
    std::shared_ptr<Impl> impl)
    : impl(std::move(impl))
{
}


RepositoryStructure::TemporarySnapshot::~TemporarySnapshot()
{
}

// TODO: Should lock the breadcrumb file on filesystem.
void RepositoryStructure::TemporarySnapshot::commit(
    RepositoryStructure const &repositoryStructure,
    BackendDriver::SnapshotId::Encoded const &encoded)
{
    auto const rename_to = repositoryStructure.pathForNode(encoded.node);

    ioutil::writefile(impl->getIdFile(), encoded.serialise(), 0666);

    try
    {
        diffmonger::rename(impl->getPath(), rename_to);
    } catch (std::system_error const &e)
    {
        if (std::filesystem::exists(rename_to))
            throw std::runtime_error(
                (std::ostringstream{}
              << "Node "
              << encoded.node.value
              << "already exists in repository; this is unexpected").str());
        throw;
    }
}


RepositoryStructure::TemporarySnapshot
RepositoryStructure::checkOutTemporarySnapshot()
{
    if (temporarySnapshotImpl)
        throw std::logic_error("TemporarySnapshot already checked out");
    temporarySnapshotImpl = std::make_shared<TemporarySnapshot::Impl>(*this);
    return TemporarySnapshot{temporarySnapshotImpl};
}


void RepositoryStructure::commit(TemporarySnapshot temporarySnapshot,
                                 BackendDriver::SnapshotId::Encoded const &encoded)
{
    if (temporarySnapshotImpl != temporarySnapshot.impl)
        throw std::invalid_argument("Invalid TemporarySnapshot: owner mismatch");
    this->temporarySnapshotImpl.reset();

    temporarySnapshot.commit(*this, encoded);

    // Note: it is okay if the following never happens (e.g., due to unexpected interrupt,
    // poweroff, etc). The Controller doesn't rely upon the breadcrumb for correctness,
    // only for efficiency. If the breadcrumb is stale, the Controller will safely
    // traverse until it reaches the end.
    writeBreadcrumb(
        Breadcrumb { .untrusted_begin = encoded.node.next(),
                     .last_trusted_node_authorised_at = encoded.timestamp
        });
}


std::filesystem::path RepositoryStructure::getPayloadFile(Node const node) const
{
    return pathForNode(node) / payloadFileName();
}


std::expected<BackendDriver::SnapshotId::Encoded, std::system_error>
RepositoryStructure::tryGetEncodedSnapshotId2(Node const node) const
{
    auto const bytes = ioutil::tryReadFile(pathForNode(node)/idFileName());

    if (!bytes)
        return std::unexpected(bytes.error());

    auto out = BackendDriver::SnapshotId::Encoded::deserialise(*bytes);

    if (out.node != node)
        throw std::runtime_error("Node mismatch in repository: repository in invalid state");

    return out;
}

std::optional<BackendDriver::SnapshotId::Encoded>
RepositoryStructure::tryGetEncodedSnapshotId(Node const node) const
{
    auto out = tryGetEncodedSnapshotId2(node);
    if (out)
        return *out;
    else if (out.error().code() == std::error_code{ENOENT, std::system_category()})
        // No such file: return empty, this isn't an error.
        return std::nullopt;
    else
        // Something else (permissions, IO issue, etc); this is an error.
        throw out.error();
}

BackendDriver::SnapshotId::Encoded
RepositoryStructure::getEncodedSnapshotId(Node const node) const
{
    auto out = tryGetEncodedSnapshotId2(node);
    if (out)
        return *out;
    else
        throw out.error();
}

void RepositoryStructure::withKeyPairs(
    std::function<bool(KeyPair const &)> const &f) const
{
    withKeyFiles(
        repository,
        [&] (std::filesystem::path const &path)
        {
            auto const buf =
                ioutil::read(FdOwner::from_syscall(open(path.c_str(), O_RDONLY),
                                                   "Could not open keyfile",
                                                   path.native()));
            return f(serialisation::Deserialiser{buf}.deserialise<KeyPair>());
        });
}

std::vector<std::unique_ptr<KeyPair>> RepositoryStructure::readKeyPairs() const
{
    std::vector<std::unique_ptr<KeyPair>> out;

    withKeyPairs(
        [&] (KeyPair const &keyPair)
        {
            out.emplace_back(std::make_unique<KeyPair>(keyPair));
            return false;
        });

    return out;
}

// Function called in unspecified order.
template <typename F>
void RepositoryStructure::foreachNode(F &&f) const
{
    for (auto const &entry:
             std::filesystem::directory_iterator{repository/snapshotsDirName()})
    {
        auto const filename = entry.path().filename();
        auto const &filename_str = filename.native();

        bool const isdir = entry.is_directory();

        // Some systems will produce stray files, e.g., ".DS_Store" on MacOS,
        // and so this just ignores unexpected files isn't of throwing.
        if (isdir && std::all_of(filename_str.begin(), filename_str.end(),
                                 [] (char c) { return std::isdigit(c); }))
            f(Node{std::stoull(filename_str)});
    }
}


std::vector<std::string> const &RepositoryStructure::getInitArgs() const
{
    return initargs;
}


Uuid RepositoryStructure::getRepositoryStructureFormatUuid() const
{
    return repositoryStructureFormatUuid;
}

std::vector<Node> RepositoryStructure::getNodesIncludingUntrusted(Node const end) const
{
    std::vector<Node> out;

    if (breadcrumb.untrusted_begin == Node::initial_value())
        return {};

    Node{breadcrumb.untrusted_begin.value - 1}
        .alive_after(
            [&] (Node const node)
            {
                // It is valid for the repository to not have all alive nodes
                // (so long as it has all nodes in the fenwick path).
                // This can happen if poweroff occurred after pruning but before
                // taking a snapshot.
                if (nodeExists(node))
                    out.push_back(node);
            });

    [&] (Node const end)
    {
        for (auto node = breadcrumb.untrusted_begin; node != end; node = node.next())
            out.push_back(node);
    }(end == Node{0} ? getNextSnapshotNode() : std::min(end, getNextSnapshotNode()));

    std::sort(out.begin(), out.end());

    return out;
}

bool RepositoryStructure::nodeExists(Node const node) const
{
    return std::filesystem::exists(pathForNode(node));
}

FdOwner RepositoryStructure::createPayloadFdForReading(Node const node) const
{
    auto const path = pathForNode(node)/payloadFileName();

    return FdOwner::from_syscall(open(path.c_str(), O_RDONLY), "open", path.c_str());
}


RepositoryStructure RepositoryStructure::createRepository(
    std::filesystem::path const &repository_path,
    Uuid const &repositoryStructureFormatUuid,
    std::vector<std::string> const &initargs)
{
    if (initargs.empty())
        throw std::invalid_argument("Invalid initargs (empty)");

    if (std::filesystem::exists(repository_path) &&
        !std::filesystem::is_empty(repository_path))
        throw std::runtime_error("Repository already exists");

    std::filesystem::create_directories(repository_path);
    std::filesystem::create_directories(repository_path/snapshotsDirName());
    std::filesystem::create_directories(repository_path/userDataDirName());
    std::filesystem::create_directories(repository_path/keysDirName());

    ioutil::writefile(repository_path/uuidFileName(),
                      Uuid::random().serialised(),
                      S_IRUSR | S_IWUSR,
                      true);

    {
        std::vector<std::byte> buffer;
        serialisation::Serialiser{buffer}
            .serialise(repositoryStructureFormatUuid)
            .serialise(std::span(initargs));
        ioutil::writefile(repository_path/initArgsFileName(),
                          buffer,
                          S_IRUSR | S_IWUSR,
                          true);
    }

    return RepositoryStructure{repository_path};
}

void RepositoryStructure::deleteNode(Node const node)
{
    std::filesystem::remove_all(pathForNode(node));
}

Uuid RepositoryStructure::getRepositoryUuid() const
{
    auto const path = repository/uuidFileName();
    auto fd = FdOwner::from_syscall(open(path.c_str(), O_RDONLY), "open", path.native());
    std::array<std::byte, Uuid::size> bytes;
    ioutil::readBytesExact(fd, bytes);
    return Uuid::deserialise(bytes);
}


std::optional<BackendDriver::SnapshotId::Encoded>
RepositoryStructure::getNodeRelativeToLastTrusted(
    std::make_signed_t<Node::value_t> const offset) const
{
    using signed_t = std::make_signed_t<Node::value_t>;
    using signedlimits = std::numeric_limits<signed_t>;
    // Taking into account that breadcrumb.untrusted_begin is +1 from the current node.
    // Note: arithmetic must not overflow; offset is allowed to be signedlimits::max()
    // and signedlimits::min().
    auto const offset_prime = std::max(signedlimits::min() + 1,
                                       offset) - 1;

    if (offset_prime > 0)
    {
        auto next = tryGetEncodedSnapshotId(breadcrumb.untrusted_begin);
        for (size_t i=0; next && (i != size_t(offset_prime)); ++i)
            next = tryGetEncodedSnapshotId(next->node.next());
        return next;
    } else
    {
        if (breadcrumb.untrusted_begin.value > signedlimits::max())
            throw std::runtime_error("Node too big");
        // Important: do /not/ do -offset_prime <= breadcrumb.untrusted_begin.value:
        // -offset_prime may overflow, whereas -breadcrumb.untrusted_begin.value
        // shall not (due to above check).
        if (offset_prime >= -static_cast<signed_t>(breadcrumb.untrusted_begin.value))
            return tryGetEncodedSnapshotId(Node{breadcrumb.untrusted_begin.value + offset});
        else
            return std::nullopt;
    }
}


RepositoryStructure::Breadcrumb const &RepositoryStructure::getBreadcrumb() const
{
    return breadcrumb;
}

void RepositoryStructure::writeBreadcrumb(Breadcrumb const &breadcrumb)
{
    std::vector<std::byte> bytes;

    serialisation::Serialiser{bytes}
        .serialise(breadcrumb.untrusted_begin.value)
        .serialise(breadcrumb.last_trusted_node_authorised_at);

    ioutil::writefile(repository/breadcrumbFileName(), bytes, 0666);

    // Pointless copy to get compiler to warn on missing fields.
    this->breadcrumb = Breadcrumb{breadcrumb.untrusted_begin,
                                  breadcrumb.last_trusted_node_authorised_at};
}

Node RepositoryStructure::getNextSnapshotNode() const
{
    Node prev = breadcrumb.untrusted_begin;
    while (true)
        if (auto const next = tryGetEncodedSnapshotId(prev))
            prev = next->node;
        else
            return prev;
}


std::filesystem::path const &RepositoryStructure::getRepository() const { return repository; }

void RepositoryStructure::storeKeyPair(std::filesystem::path const &repository_path,
                                       KeyPair const &keyPair)
{
    auto const paths = getKeyFiles(repository_path);

    size_t const slot = paths.size();

    auto const path = repository_path/keysDirName()/std::to_string(slot);

    auto fd = FdOwner::from_syscall(open(path.c_str(),
                                         O_CREAT|O_WRONLY|O_CLOEXEC|O_EXCL,
                                         S_IRUSR|S_IWUSR),
                                    "open keyfile", path.native());
    ioutil::write(fd, keyPair.serialise());
}


RepositoryStructure::RepositoryStructure(std::filesystem::path const &repository)
    : lockfd{
            [&]
            {
                auto const lockpath = repository/lockFileName();
                auto out =
                    FdOwner::from_syscall(open(lockpath.c_str(),
                                               O_CREAT | O_TRUNC | O_RDWR,
                                               0666),
                                          lockpath.c_str());
                flock fl = {
                    .l_type = F_WRLCK,
                    .l_whence = SEEK_SET,
                    .l_start = 0,
                    .l_len = 0,
                    .l_pid = 0 // Unused
                };

                while (true)
                    // Blocks.
                    if (int const r = fcntl(out.get(), F_OFD_SETLKW, &fl);
                        r == 0)
                        return out;
                    else
                    {
                        if (errno != EINTR)
                            throw std::system_error(errno, std::system_category(),
                                                    "Failed to obtain lock");
                    }
                return out;
            }()},
      repository(repository)
{
    {
        auto const data = ioutil::readfile(repository / initArgsFileName());
        serialisation::Deserialiser{data}
            .deserialise(repositoryStructureFormatUuid)
            .deserialise(initargs);
    }

    if (initargs.empty())
        throw std::runtime_error("Repository initargs were non-valid (empty)");

    {
        if (auto const breadcrumbPath = repository/breadcrumbFileName();
            std::filesystem::exists(breadcrumbPath))
        {
            auto const bytes = ioutil::readfile(breadcrumbPath);
            serialisation::Deserialiser{bytes}
                .deserialise(breadcrumb.untrusted_begin.value)
                .deserialise(breadcrumb.last_trusted_node_authorised_at);
            // Pointless copy to get compiler to warn on missing fields.
            breadcrumb = Breadcrumb{breadcrumb.untrusted_begin,
                                    breadcrumb.last_trusted_node_authorised_at};
        } else
        {
            breadcrumb = Breadcrumb{Node{0},
                                    BackendDriver::SnapshotId::timestamp_t::min()};
        }
    }
}


RepositoryStructure::~RepositoryStructure() {}


std::filesystem::path RepositoryStructure::pathForNode(Node const node) const
{
    return repository/snapshotsDirName()/std::to_string(node.value);
}

}
