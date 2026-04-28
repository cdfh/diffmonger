#include "zfs.hpp"
#include "sysutil.hpp"

#include <diffmonger/util/misc.hpp>

#include <regex>
#include <algorithm>
#include <cassert>


namespace diffmonger {
namespace zfs {
namespace {
bool verify_guid(std::string const &guid)
{
    return !guid.empty() && std::all_of(guid.begin(), guid.end(),
                                        [] (char x) { return std::isdigit(x); });
}
}



DatasetName DatasetName::operator/(std::string_view const suffix) const
{
    return { concat(dataset, "/", suffix) };
}

DatasetName::DatasetName(std::string const &str) : dataset(str)
{
    static const std::regex regex(R"(^[a-zA-Z0-9_\-\.]+(/[a-zA-Z0-9_\-\.]+)*$)");

    if (!std::regex_match(dataset, regex))
        throw std::invalid_argument("Invalid dataset name: " + dataset);
}

SnapshotName::SnapshotName(std::string const &str) : snapshot(str)
{
    // Note: Syncoid sadly uses ':' in its snapshot names, so disallowing ':' will cause
    // runtime_errors when importing snapshots if the user is running syncoid.
    static const std::regex regex(R"(^[a-zA-Z][a-zA-Z0-9_\-\.:]{0,254}$)");

    if (!std::regex_match(snapshot, regex))
        throw std::invalid_argument("Invalid snapshot name: " + snapshot);
}

SnapshotGuid::SnapshotGuid(std::string const &guid)
    : guid(
        [&guid]
        {
            if (!std::all_of(guid.begin(), guid.end(), [] (char c) { return std::isdigit(c); }))
                throw std::invalid_argument("Invalid guid: " + guid);
            uint64_t const out = std::stoull(guid);
            // There's no reason the guid can't be null,
            // but it's astronomically unlikely to be so.
            // If this fails, then either it's a bug, or the end of the universe is near.
            if (!out)
                throw std::invalid_argument("Invalid null guid");
            return out;
        }())
{
    assert(std::to_string(this->guid) == guid);
}

DatasetSnapshotName DatasetSnapshotName::fromString(std::string_view const str)
{
    auto const it = std::find(str.begin(), str.end(), '@');
    if (it == str.end())
        throw std::invalid_argument(std::string("Invalid DatesetSnapshotName: ").append(str));
    return { DatasetName(std::string(str.begin(), it)),
             SnapshotName(std::string(std::next(it), str.end())) };
}

std::string DatasetSnapshotName::str(DatasetName const &dataset,
                                     SnapshotName const &snapshot)
{
    return concat(dataset.str(), "@", snapshot.str());
}

void zfs_snapshot(Command zfsCommand,
                  DatasetName const &dataset,
                  SnapshotName const &snapshot)
{
    zfsCommand.extend("snapshot", DatasetSnapshotName(dataset, snapshot).str());

    with_null_terminated_array_of_cstrs2(
        zfsCommand.args,
        [&] (auto args)
        { systemvp_throw_on_failure(zfsCommand.cmd.c_str(), args); });
}

SnapshotGuid zfs_get_guid(Command zfsCommand, DatasetSnapshotName const &snapshot)
{
    zfsCommand.extend("get", "-H", "-o", "value", "guid", snapshot.str());

    return with_null_terminated_array_of_cstrs2(
        zfsCommand.args,
        [&] (auto args)
        {
            auto const guid = systemvp_strchomp(zfsCommand.cmd.c_str(), args);
            if (!verify_guid(guid))
                throw std::runtime_error("zfs reported an invalid guid! (" + guid + ")");
            return SnapshotGuid{guid};
        });
}

std::vector<SnapshotNameGuid> zfs_get_snapshots(Command zfsCommand,
                                                DatasetName const &dataset)
{
    zfsCommand.extend("list", "-t", "snapshot", "-o", "name,guid", "-H", dataset.str());

    auto const str =
        with_null_terminated_array_of_cstrs2(
            zfsCommand.args,
            [&] (auto args)
            { return systemvp_str(zfsCommand.cmd.c_str(), args); });

    std::vector<SnapshotNameGuid> out;

    for (auto it = str.begin(); it != str.end(); )
    {
        auto const it_newline = std::find(it, str.end(), '\n');

        if (it_newline == str.end())
            throw std::runtime_error("zfs list: output malformed; no trailing newline");
        else if (it_newline == it)
            throw std::runtime_error("zfs list: output malformed; empty snapshot name");

        auto const line = std::string_view(it, it_newline);

        // The documentation explicitly states that -H causes tab separation.
        auto const it_tab = std::find(line.begin(), line.end(), '\t');

        if (it_tab == line.end())
            throw std::runtime_error(
                std::string("zfs list: output malformed; no guid given: ").append(line));

        auto const datasetSnapshotName =
            DatasetSnapshotName::fromString(std::string_view(line.begin(), it_tab));
        auto const guid = SnapshotGuid(std::string(std::next(it_tab), line.end()));

        if (!(datasetSnapshotName.dataset == dataset))
            throw std::runtime_error(std::string("zfs list: zfs produced invalid output: ")
                                     .append(line));
        out.emplace_back(SnapshotNameGuid{datasetSnapshotName.snapshot, guid });
        it = std::next(it_newline);
    }

    return out;
}

void zfs_diffsend(Command zfsCommand,
                  DatasetName const &dataset,
                  SnapshotName const &basename,
                  SnapshotName const &curname,
                  FdOwner out)
{
    auto const basename_str = "@" + basename.str();
    auto const curname_str = DatasetSnapshotName(dataset, curname).str();

    zfsCommand.extend("send", "-i", basename_str, curname_str);

    with_null_terminated_array_of_cstrs2(
        zfsCommand.args,
        [&] (auto args) { systemvp_throw_on_failure(zfsCommand.cmd.c_str(), args, std::move(out)); });
}

void zfs_send(Command zfsCommand,
              DatasetName const &dataset,
              SnapshotName const &snapshot,
              FdOwner out)
{
    zfsCommand.extend("send", DatasetSnapshotName(dataset, snapshot).str());

    with_null_terminated_array_of_cstrs2(
        zfsCommand.args,
        [&] (auto args) { systemvp_throw_on_failure(zfsCommand.cmd.c_str(), args, std::move(out)); });
}

void zfs_receive(Command zfsCommand,
                 DatasetName const &dataset,
                 SnapshotName const &snapshot,
                 FdOwner in,
                 std::span<std::string const> args)
{
    zfsCommand.extend("receive");
    zfsCommand.args.insert(zfsCommand.args.end(), args.begin(), args.end());
    zfsCommand.extend(DatasetSnapshotName(dataset, snapshot).str());

    with_null_terminated_array_of_cstrs2(
        zfsCommand.args,
        [&] (auto args) { systemvp_throw_on_failure3(zfsCommand.cmd.c_str(), args, std::move(in)); });
}

void zfs_remove_snapshot(Command zfsCommand,
                         DatasetName const &dataset,
                         SnapshotName const &snapshot)
{
    zfsCommand.extend("destroy", DatasetSnapshotName(dataset, snapshot).str());

    with_null_terminated_array_of_cstrs2(
        zfsCommand.args,
        [&] (auto args) { systemvp_throw_on_failure(zfsCommand.cmd.c_str(), args); });
}

void zfs_destroy_dataset(Command zfsCommand,
                         DatasetName const &dataset)
{
    zfsCommand.extend("destroy", dataset.str());

    with_null_terminated_array_of_cstrs2(
        zfsCommand.args,
        [&] (auto args) { systemvp_throw_on_failure(zfsCommand.cmd.c_str(), args); });
}

void zfs_rollback(Command zfsCommand, DatasetSnapshotName const &datasetSnapshot)
{
    zfsCommand.extend("rollback", "-r", datasetSnapshot.str());

    with_null_terminated_array_of_cstrs2(
        zfsCommand.args,
        [&] (auto args) { systemvp_throw_on_failure(zfsCommand.cmd.c_str(), args); });
}

std::string zfs_version_info(Command zfsCommand)
{
    zfsCommand.extend("--version");
    return
        with_null_terminated_array_of_cstrs2(
            zfsCommand.args,
            [&] (auto args) { return systemvp_str(zfsCommand.cmd.c_str(), args); });
}

}}
