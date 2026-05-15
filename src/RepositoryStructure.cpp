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
#include <utility>
#include <algorithm>
#include <sys/stat.h>
#include <charconv>

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
#if 0
inline std::filesystem::path repositoryVersionFileName() { return "repository-version"; }
inline std::string_view repositoryVersion() { return "fb95347a-5159-4e50-8c63-8757a96550e4"; }
#endif
inline std::filesystem::path uuidFileName() { return "repository-uuid"; }

static constexpr size_t maxKeySlots = 255;


// Returns the number of keyFiles present.
template <typename F>
void withKeyFiles(std::filesystem::path const &repository, F &&f)
{
    auto const dir = repository/keysDirName();

    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
        throw std::runtime_error("Invalid keys directory for repository");

    /*
     * The logic here is unexpected but intentional.
     * We cannot do a regular traversal of the directory because an attacker
     * may have made the directory infinitely populated in effort to hinder recovery.
     * Thus, we iterate on a counter instead.
     */

    size_t n = 0;
    for (auto const &ignored: std::filesystem::directory_iterator(dir))
    {
        ++n;
        (void) ignored;
    }

    size_t const counter_end = std::min(n, maxKeySlots + 1);
    for (size_t counter = 0; counter != counter_end; ++counter)
    {
        auto const filename = dir/std::to_string(counter);
        if (std::filesystem::exists(filename))
        {
            if (f(filename, counter))
                return;
        }
    }

    if (n > maxKeySlots)
        throw std::runtime_error("Too many keyslots");
}


std::vector<std::pair<std::filesystem::path, size_t>>
getKeyFiles(std::filesystem::path const &repository)
{
    struct Entry
    {
        Entry(std::filesystem::path const &path) : path(path)
        {
            auto const &filename = this->path.filename();
            auto const &str = filename.native();
            if (std::from_chars(str.data(), str.data() + str.size(), n).ec != std::errc{})
                throw std::runtime_error("Could not interpret filename as a key: " + str);
        }
        bool operator<(Entry const &other) const
        {
            return n < other.n;
        }
        std::filesystem::path path;
        size_t n;
    };

    std::vector<Entry> entries;

    withKeyFiles(
        repository,
        [&] (std::filesystem::path const &path, size_t)
        {
            entries.push_back(path);
            return false;
        });

    std::sort(entries.begin(), entries.end());

    std::vector<std::pair<std::filesystem::path, size_t>> out;
    for (auto const &entry: entries)
        out.emplace_back(entry.path, entry.n);

    return out;
}

}

struct RepositoryStructure::TemporarySnapshot::Impl
{
    std::filesystem::path repository;
    FdOwner fd;
    std::optional<BackendDriver::SnapshotId::Encoded> encoded;

    Impl(std::filesystem::path const &repository)
        : repository(repository)
    {
        auto const path = getPath();
        auto const payloadFile = path/payloadFileName();
        if (std::filesystem::exists(path))
        {
            std::filesystem::remove(path/idFileName());
            std::filesystem::remove(payloadFile);
        } else
        {
            std::filesystem::create_directories(path);
        }

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

RepositoryStructure::TemporarySnapshot::TemporarySnapshot(TemporarySnapshot &&) = default;
RepositoryStructure::TemporarySnapshot &
RepositoryStructure::TemporarySnapshot::operator=(
    RepositoryStructure::TemporarySnapshot &&) = default;


FdOwner &RepositoryStructure::TemporarySnapshot::getFd() { return impl->fd; }

RepositoryStructure::TemporarySnapshot::TemporarySnapshot(
    std::filesystem::path const &repository)
    : impl(std::make_unique<Impl>(repository))
{
}

RepositoryStructure::TemporarySnapshot::~TemporarySnapshot()
{
};

void RepositoryStructure::TemporarySnapshot::commit(
    std::filesystem::path const &rename_to,
    BackendDriver::SnapshotId::Encoded const &encoded)
{
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
    std::function<bool(KeyPair const &, size_t)> const &f) const
{
    withKeyFiles(
        repository,
        [&] (std::filesystem::path const &path, size_t const slot)
        {
            auto const buf =
                ioutil::read(FdOwner::from_syscall(open(path.c_str(), O_RDONLY),
                                                   "Could not open keyfile",
                                                   path.native()));
            return f(serialisation::Deserialiser{buf}.deserialise<KeyPair>(), slot);
        });
}

std::vector<std::unique_ptr<KeyPair>> RepositoryStructure::readKeyPairs() const
{
    std::vector<std::unique_ptr<KeyPair>> out;

    withKeyPairs(
        [&] (KeyPair const &keyPair, size_t)
        {
            out.emplace_back(std::make_unique<KeyPair>(keyPair));
            return false;
        });

    return out;
}

void RepositoryStructure::commit(TemporarySnapshot temporarySnapshot,
                                 BackendDriver::SnapshotId::Encoded const &encoded)
{
    if (temporarySnapshot.impl->repository != repository)
        throw std::logic_error("Invalid TemporarySnapshot: repository mismatch");
    temporarySnapshot.commit(pathForNode(encoded.node), encoded);
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

std::vector<Node> RepositoryStructure::getNodes() const
{
    std::vector<Node> nodes;
    foreachNode([&nodes] (Node const node) { nodes.emplace_back(node); });
    std::sort(nodes.begin(), nodes.end());

    return nodes;
}

std::optional<Node> RepositoryStructure::getLatestNode() const
{
    std::optional<Node> out;

    foreachNode(
        [&] (Node const node)
        {
            if (out)
                out = std::max(*out, node);
            else
                out = node;
        });

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

RepositoryStructure::TemporarySnapshot RepositoryStructure::createTemporarySnapshot()
{
    return TemporarySnapshot{repository};
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

#if 0
    ioutil::writefile(repository_path/repositoryVersionFileName(),
                      std::as_bytes(std::span(repositoryVersion())),
                      S_IRUSR | S_IWUSR,
                      true);
#endif

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

    return { repository_path };
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

std::filesystem::path const &RepositoryStructure::getRepository() const { return repository; }

void RepositoryStructure::storeKeyPair(std::filesystem::path repository_path,
                                       KeyPair const &keyPair)
{
    auto const paths = getKeyFiles(repository_path);

    size_t slot = paths.empty() ? 0 : paths.back().second + 1;

    auto const path = repository_path/keysDirName()/std::to_string(slot);

    auto fd = FdOwner::from_syscall(open(path.c_str(),
                                         O_CREAT|O_WRONLY|O_CLOEXEC|O_EXCL,
                                         S_IRUSR|S_IWUSR),
                                    "open keyfile", path.native());
    ioutil::write(fd, keyPair.serialise());
}


// Copy constructor.
RepositoryStructure::RepositoryStructure(RepositoryStructure const &other)
    : RepositoryStructure(other.repository)
{
}

RepositoryStructure::RepositoryStructure(std::filesystem::path const &repository)
    : repository(repository)
{
#if 0
    auto const thisRepositoryVersion =
        ioutil::readfile(repository/repositoryVersionFileName());
    if (!equal(std::as_bytes(std::span(thisRepositoryVersion)),
               std::as_bytes(std::span(repositoryVersion()))))
        throw std::runtime_error(
            std::string("Unsuported diffmonger repository version: ")
            .append(std::string_view(
                        reinterpret_cast<char const *>(thisRepositoryVersion.data()),
                        thisRepositoryVersion.size())));
#endif
    auto const data = ioutil::readfile(repository / initArgsFileName());
    serialisation::Deserialiser{data}
        .deserialise(repositoryStructureFormatUuid)
        .deserialise(initargs);

    if (initargs.empty())
        throw std::runtime_error("Repository initargs were non-valid (empty)");
}

RepositoryStructure::~RepositoryStructure() {}



std::filesystem::path RepositoryStructure::pathForNode(Node const node) const
{
    return repository/snapshotsDirName()/std::to_string(node.value);
}

}
