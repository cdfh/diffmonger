#ifndef DIFFMONGER_ZFS_HPP
#define DIFFMONGER_ZFS_HPP

#include <diffmonger/util/FdOwner.hpp>
#include <diffmonger/util/PidOwner.hpp>

#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#include <span>

namespace diffmonger {
namespace zfs {

class DatasetName
{
public:
    DatasetName() = delete;
    DatasetName(std::string const &str);

    std::string const &str() const
    {
        return dataset;
    }

    bool operator==(DatasetName const &other) const
    {
        return dataset == other.dataset;
    }

    DatasetName operator/(std::string_view suffix) const;

private:
    std::string dataset;
};

class SnapshotName
{
public:
    SnapshotName() = delete;
    SnapshotName(std::string const &str);

    std::string const &str() const
    {
        return snapshot;
    }
private:
    std::string snapshot;
};

class SnapshotGuid
{
public:
    SnapshotGuid() = delete;
    SnapshotGuid(std::string const &guid);

    std::string str() const
    {
        return std::to_string(guid);
    }

    auto operator<=>(SnapshotGuid const &) const = default;
private:
    uint64_t guid;
};

struct SnapshotNameGuid
{
    SnapshotName name;
    SnapshotGuid guid;
};

struct DatasetSnapshotName
{
    DatasetSnapshotName(DatasetName const &dataset, SnapshotName const &snapshot)
        : dataset(dataset),
          snapshot(snapshot)
    {}

    static DatasetSnapshotName fromString(std::string_view str);
    static std::string str(DatasetName const &dataset, SnapshotName const &snapshot);

    std::string str() const
    {
        return str(dataset, snapshot);
    }

    DatasetName dataset;
    SnapshotName snapshot;
};

struct Command
{
    std::string cmd;
    std::vector<std::string> args;

    Command() = delete;

    Command(std::string cmd, std::vector<std::string> args) : cmd(std::move(cmd)),
                                                              args(std::move(args))
    {
        if (this->args.empty())
            throw std::invalid_argument("Command: invalid arguments (empty)");
    }

    Command(std::string const &cmd) : Command(cmd, { cmd }) {}
    Command(std::span<std::string const> const args)
        : Command(args.empty()
                  ? throw std::invalid_argument("Command: invalid arguments (empty)")
                  : args.front(),
                  {args.begin(), args.end()})
    {}

    template <typename ...T>
    Command &extend(T && ...xs)
    {
        (args.push_back(std::forward<T>(xs)), ...);
        return *this;
    }

    template <typename ...T>
    [[nodiscard]] Command extended(T && ...xs) const
    {
        return Command(*this).extend(std::forward<T>(xs)...);
    }
};

std::vector<SnapshotNameGuid> zfs_get_snapshots(Command zfsCommand,
                                                DatasetName const &dataset);

void zfs_snapshot(Command zfsCommand,
                  DatasetName const &dataset,
                  SnapshotName const &snapshot);

/**
 * Forcefully take a snapshot: like zfs_snapshot(),
 * but if an existing snapshot has the same name, destroys the existing snapshot first.
 */
void zfs_forcefully_snapshot(Command zfsCommand,
                             DatasetName const &dataset,
                             SnapshotName const &snapshot);

SnapshotGuid zfs_get_guid(Command zfsCommand, DatasetSnapshotName const &snapshot);

void zfs_diffsend(Command zfsCommand,
                  DatasetName const &dataset,
                  SnapshotName const &base_name,
                  SnapshotName const &cur_name,
                  FdOwner out);

void zfs_send(Command zfsCommand,
              DatasetName const &dataset,
              SnapshotName const &snapshot,
              FdOwner out);

void zfs_receive(Command zfsCommand,
                 DatasetName const &dataset,
                 SnapshotName const &snapshot,
                 FdOwner in,
                 std::span<std::string const> args = {});

void zfs_remove_snapshot(Command zfsCommand,
                         DatasetName const &dataset,
                         SnapshotName const &snapshot);

void zfs_destroy_dataset(Command zfsCommand,
                         DatasetName const &dataset);

void zfs_rollback(Command zfsCommand, DatasetSnapshotName const &datasetSnapshot);

std::string zfs_version_info(Command zfsCommand);

}}


#endif
