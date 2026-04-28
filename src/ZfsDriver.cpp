#include "ZfsDriver.hpp"
#include "zfs.hpp"

#include <diffmonger/util/Serialisation.hpp>
#include <diffmonger/util/Uuid.hpp>

#include <optional>
#include <string>
#include <vector>
#include <stdexcept>
#include <sys/types.h>

namespace diffmonger {

using namespace zfs;

namespace {

/*
 * If ever the driver changes in a way that changes binary compatibility of
 * encoded objects, a new UUID needs to be generated.
 * It would be fine to just use linear increments.
 */
static Uuid driverIdentityAndVersion()
{
    return Uuid{
        std::to_array<uint8_t>(
            {0xe3,0x7c,0xfb,0xbe,0x35,0x16,0xaf,0x29,
             0xf4,0x26,0x7e,0xc1,0xdf,0x50,0xc1,0x61})};
}

struct SnapshotIdImpl final : public BackendDriver::SnapshotId
{
    Node node;
    SnapshotGuid guid;
    SnapshotName name;
    timestamp_t timestamp;

    template <typename Guid, typename Name>
    SnapshotIdImpl(
        Node const node,
        Guid &&guid,
        Name &&name,
        timestamp_t const timestamp = BackendDriver::SnapshotId::timestamp_t::clock::now())
        : node(node),
          guid(std::forward<Guid>(guid)),
          name(std::forward<Name>(name)),
          timestamp(timestamp)
    {}

    std::string pretty() const override
    {
        return name.str();
    }

    Encoded encoded() const override
    {
        std::vector<std::byte> bytes;

        Serialiser serialiser(bytes);

        serialiser.serialise(driverIdentityAndVersion().serialised());
        serialiser.serialise(guid.str());
        serialiser.serialise(name.str());

        return Encoded { .bytes = std::move(bytes),
                         .node = node,
                         .timestamp = timestamp };
    }

    Node getNode() const override
    {
        return node;
    }

    bool getTimestamp(timestamp_t &out) const override
    {
        out = timestamp;
        return true;
    }

    // Parses the encoded representation and constructs a SnapshotIdImpl,
    // but does not verify that the snapshot exists in zfs.
    // This method should be used with caution; normally the existence of a SnapshotId
    // instance is meant to imply that the snapshot physically exists.
    static std::unique_ptr<SnapshotIdImpl>
    parseNoVerify(Encoded const &encoded)
    {
        Deserialiser deserialiser(encoded.bytes);

        Uuid::array_type driverIdentityAndVersionValue;
        std::string guid;
        std::string name;

        deserialiser.deserialise(driverIdentityAndVersionValue);
        deserialiser.deserialise(guid);
        deserialiser.deserialise(name);

        if (Uuid{driverIdentityAndVersionValue} != driverIdentityAndVersion())
            throw std::runtime_error(
                "Driver identity or version mismatch when decoding encoded snapshot");

        return std::make_unique<SnapshotIdImpl>(
            encoded.node,
            SnapshotGuid{guid},
            SnapshotName{name},
            encoded.timestamp);
    }
};
}

class ZfsBackendDriverImpl : public ZfsBackendDriver
{
    SnapshotIdImpl const &castAndVerify(SnapshotId const &snapshotId) const
    {
        auto const &snapshotIdImpl = dynamic_cast<SnapshotIdImpl const &>(snapshotId);

        auto const guid =
            [&]
            {
                try
                {
                    return zfs_get_guid(zfsCommand, { dataset, snapshotIdImpl.name });
                } catch (command_failed const &err)
                {
                    throw SnapshotDoesNotExist{snapshotIdImpl};
                }
            }();

        if (guid != snapshotIdImpl.guid)
            throw SnapshotInvalid(snapshotIdImpl,
                                  "Snapshot GUID mismatch; "
                                  "the snapshot has changed since being created "
                                  "(expected guid: " + snapshotIdImpl.guid.str() +
                                  "; actual guid: " + guid.str() + ").");

        return snapshotIdImpl;
    }

    void verify(SnapshotId const &snapshotId) const override
    {
        castAndVerify(snapshotId);
    }

    bool exists(SnapshotId const &snapshotId) const override
    {
        try
        {
            castAndVerify(snapshotId);
            return true;
        } catch (SnapshotDoesNotExist const &e)
        {
            return false;
        }
    }


public:
    std::pair<std::unique_ptr<SnapshotId>, bool> snapshot(
        Node const node,
        Uuid const &uuid) override
    {
        SnapshotName const name{
            (std::ostringstream{}
               << "diffmonger-"
               << node.value
               << "-"
               << uuid.ascii()).str()
        };

        std::exception_ptr maybe_exception;

        try
        {
            zfs_snapshot(zfsCommand, dataset, name);
        } catch (std::system_error const &e)
        {
            // If this throws, perhaps it's because the snapshot already existed.
            // Assume that is the case and continue.
            // If the assumption is false, then zfs_get_guid(), called next,
            // will certainly throw, and we'll rethrow the current exception.
            maybe_exception = std::current_exception();
        }

        try
        {
            // If this doesn't throw, then the snapshot already existed.
            auto const guid = zfs_get_guid(zfsCommand, { dataset, name });
            bool const already_existed = static_cast<bool>(maybe_exception);
            return std::pair(std::make_unique<SnapshotIdImpl>(node, guid, name),
                             already_existed);
        } catch (std::runtime_error const &)
        {
            if (maybe_exception)
                std::rethrow_exception(maybe_exception);
            throw;
        }
    }

    void remove_snapshot(SnapshotId const &snapshotId) override
    {
        zfs_remove_snapshot(zfsCommand, dataset, castAndVerify(snapshotId).name);
    }

    void initial(SnapshotId const &snapshotId, FdOwner out) override
    {
        zfs_send(zfsCommand, dataset, castAndVerify(snapshotId).name, std::move(out));
    }

    void diff(SnapshotId const &fromSnapshotId, SnapshotId const &toSnapshotId, FdOwner out) override
    {
        // Note: do /not/ use zfs send -I (instead of -i);
        // this would result in diffmonger storing some nodes multiple times.
        // Diffmonger stores redundant nodes beyond the strict Fenwick ancestors.
        // If -I is given, these redundant nodes will be stored in the stream,
        // but that's wasteful, since Diffmonger already directly stores these nodes as their
        // own streams.
        zfs_diffsend(zfsCommand,
                     dataset,
                     castAndVerify(fromSnapshotId).name,
                     castAndVerify(toSnapshotId).name,
                     std::move(out));
    }

    void restoreInitial(SnapshotId const &snapshotId, FdOwner in) override
    {
        std::vector<std::string> args = { "-F" };

        if (receive0_args)
            args.insert(args.end(), receive0_args->begin(), receive0_args->end());

        zfs_receive(zfsCommand,
                    dataset,
                    dynamic_cast<SnapshotIdImpl const &>(snapshotId).name,
                    std::move(in),
                    args);
    }

    void restoreDiff(SnapshotId const &fromSnapshotId,
                     SnapshotId const &toSnapshotId,
                     FdOwner in) override
    {
        castAndVerify(fromSnapshotId);

        std::vector<std::string> args = { "-F" };

        if (receiven_args)
            args.insert(args.end(), receiven_args->begin(), receiven_args->end());

        zfs_receive(zfsCommand,
                    dataset,
                    dynamic_cast<SnapshotIdImpl const &>(toSnapshotId).name,
                    std::move(in),
                    args);
    }

    std::unique_ptr<SnapshotId>
    snapshotId(SnapshotId::Encoded const &encoded) const override
    {
        return SnapshotIdImpl::parseNoVerify(encoded);
    }

    std::string version() const override
    {
        return zfs_version_info(zfsCommand);
    }

    ZfsBackendDriverImpl(DriverArguments args)
        : zfsCommand(std::move(args.zfsCommand)),
          dataset{std::move(args.datasetName)},
          receive_unmounted{args.receive_unmounted},
          receive0_args(std::move(args.receive0_args)),
          receiven_args(std::move(args.receiven_args))
    {}

protected:
    zfs::DatasetName getDataset() const { return dataset; }
    Command const &getZfsCommand() const { return zfsCommand; }
private:
    Command zfsCommand;
    zfs::DatasetName dataset;
    bool receive_unmounted;
    std::optional<std::vector<std::string>> receive0_args;
    std::optional<std::vector<std::string>> receiven_args;
};

std::unique_ptr<BackendDriver::Factory> ZfsBackendDriver::createFactory(
    RepositoryParams repositoryParams,
    DriverArguments driverArguments)
{
    struct Factory : BackendDriver::Factory
    {
        std::unique_ptr<BackendDriver> create() const override
        {
            return std::make_unique<ZfsBackendDriverImpl>(driverArguments);
        }

        Factory(RepositoryParams repositoryParams, DriverArguments driverArguments)
            : repositoryParams(std::move(repositoryParams)),
              driverArguments(std::move(driverArguments))
        {}

        RepositoryParams repositoryParams;
        DriverArguments driverArguments;
    };

    return std::make_unique<Factory>(std::move(repositoryParams),
                                     std::move(driverArguments));
}

}
