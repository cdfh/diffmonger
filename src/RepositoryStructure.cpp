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
#include <map>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <fstream>
#include <iomanip>
#include <charconv>

namespace diffmonger {

namespace {
static std::filesystem::path snapshotsDirName() { return "snapshots"; }
static std::filesystem::path temporarySnapshotDirName() { return "temporary-snapshot"; }
static std::filesystem::path payloadFileName() { return "blob"; }
static std::filesystem::path idFileName() { return "id"; }
static std::filesystem::path initArgsFileName() { return "initargs"; }
static std::filesystem::path keysDirName() { return "keys"; }
static std::filesystem::path userDataDirName() { return "user-data"; }
static std::filesystem::path repositoryVersionFileName() { return "repository-version"; }
static std::filesystem::path uuidFileName() { return "repository-uuid"; }
static std::string_view repositoryVersion() { return "fb95347a-5159-4e50-8c63-8757a96550e4"; }

std::filesystem::path getKeyFile(std::filesystem::path const &repository,
                                 bool create_new = false)
{
    auto const dir = repository/keysDirName();
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
        throw std::runtime_error("Invalid keys directory for repository");
    struct Candidate
    {
        Candidate(std::filesystem::path const &path) : path(path)
        {
            auto const &filename = this->path.filename();
            auto const &str = filename.native();
            if (std::from_chars(str.data(), str.data() + str.size(), n).ec != std::errc{})
                throw std::runtime_error("Could not interpret filename as a key: " + str);
        }
        bool operator<(Candidate const &other) const
        {
            return n < other.n;
        }
        std::filesystem::path path;
        size_t n;
    };

    std::vector<Candidate> candidates;
    for (auto const &file: std::filesystem::directory_iterator(dir))
        candidates.emplace_back(file.path());

    auto const it = std::max_element(candidates.begin(),
                                     candidates.end());

    if (create_new)
    {
        if (it == candidates.end())
            return repository/keysDirName()/"0";
        else
            return repository/keysDirName()/std::to_string(it->n + 1);
    } else
    {
        if (it == candidates.end())
            throw std::runtime_error("No keys in repository");
        return it->path;
    }
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

std::unique_ptr<KeyPair> RepositoryStructure::readKeyPair() const
{
    auto const keyFile = getKeyFile(repository);
    auto const buf =
        ioutil::read(FdOwner::from_syscall(open(keyFile.c_str(), O_RDONLY),
                                           "Could not open keyfile",
                                           keyFile.native()));
    return std::make_unique<KeyPair>(KeyPair::deserialise(buf));
}

void RepositoryStructure::commit(TemporarySnapshot temporarySnapshot,
                                 BackendDriver::SnapshotId::Encoded const &encoded)
{
    if (temporarySnapshot.impl->repository != repository)
        throw std::logic_error("Invalid TemporarySnapshot: repository mismatch");
    temporarySnapshot.commit(pathForNode(encoded.node), encoded);
}

std::vector<std::string> RepositoryStructure::getInitArgs() const
{
    auto const data = ioutil::readfile(repository / initArgsFileName());
    Deserialiser deserialiser(data);
    std::vector<std::string> out;
    deserialiser.deserialise(out);
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
    std::vector<std::string> const &initargs)
{
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

    ioutil::writefile(repository_path/repositoryVersionFileName(),
                      std::as_bytes(std::span(repositoryVersion())),
                      S_IRUSR | S_IWUSR,
                      true);

    {
        std::vector<std::byte> buffer;
        Serialiser serialiser(buffer);
        serialiser.serialise(std::span(initargs));
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

void RepositoryStructure::storeKeys(std::filesystem::path repository_path,
                                    KeyPair const &keyPair)
{
    auto const path = getKeyFile(repository_path, true);;
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
    auto const thisRepositoryVersion =
        ioutil::readfile(repository/repositoryVersionFileName());
    if (!equal(std::as_bytes(std::span(thisRepositoryVersion)),
               std::as_bytes(std::span(repositoryVersion()))))
        throw std::runtime_error(
            std::string("Unsuported diffmonger repository version: ")
            .append(std::string_view(
                        reinterpret_cast<char const *>(thisRepositoryVersion.data()),
                        thisRepositoryVersion.size())));
}

RepositoryStructure::~RepositoryStructure() {}



std::filesystem::path RepositoryStructure::pathForNode(Node const node) const
{
    return repository/snapshotsDirName()/std::to_string(node.value);
}

}
