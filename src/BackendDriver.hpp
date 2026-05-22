#ifndef DIFFMONGER_BACKEND_DRIVER_HPP
#define DIFFMONGER_BACKEND_DRIVER_HPP

#include "Node.hpp"

#include <diffmonger/util/FdOwner.hpp>

#include <string>
#include <vector>
#include <memory>
#include <span>
#include <chrono>
#include <tuple>
#include <cassert>

namespace diffmonger {

class Uuid;

// Backends: zfs, btrfs, git
class BackendDriver
{
public:
    /**
     * Identifies a snapshot.
     * Some implementations encode integrity-checking information within the SnapshotId,
     * and so to ensure consistent usage, this type is made opaque.
     */
    class SnapshotId
    {
    public:
        /**
         * Timestamp type.
         * Thankfully, the C++ standard now defines the epoch of std::chrono::sys_time
         * (https://eel.is/c++draft/time.clock.system#overview-1),
         * and so it is not implementation defined:
         *
         * > Objects of type system_clock represent wall clock time from the
         * > system-wide realtime clock. Objects of type sys_time<Duration> measure
         * > time since 1970-01-01 00:00:00 UTC excluding leap seconds.
         * > This measure is commonly referred to as Unix time.
         * > This measure facilitates an efficient mapping between sys_time
         * > and calendar types ([time.cal]).
         */
        using timestamp_t = std::chrono::sys_time<std::chrono::nanoseconds>;

        static_assert(std::is_same_v<timestamp_t::rep, int64_t>);

        /**
         * A (mostly) serialised representation of a SnapshotId.
         */
        struct Encoded
        {
            /**
             * Serialised snapshot identifier.
             * The BackendDriver must be able to verify that the contained information
             * was created by a version of itself that it is compatible with.
             */
            std::vector<std::byte> bytes;
            Node node;
            timestamp_t timestamp;

            auto operator<=>(Encoded const &) const = default;

            std::vector<std::byte> serialise() const;
            static Encoded deserialise(std::span<std::byte const> bytes);
        };

        /**
         * A pretty representation for human interpretation.
         */
        virtual std::string pretty() const = 0;

        /**
         * Like pretty(), but with extra diagnostic information, if available.
         * This is used when there's a runtime error that the user may need to investigate.
         */
        virtual std::string pretty_diagnostics() const { return pretty(); }

        /**
         * A low-level binary representation from which a SnapshotId can be reconstructed via
         * BackendDriver::snapshotId().
         */
        virtual Encoded encoded() const = 0;

        virtual Node getNode() const = 0;

        /**
         * Gets the UTC timestamp at which the underlying snapshot was
         * created and returns true,
         * or returns false if the timestamp is unknown.
         */
        virtual bool getTimestamp(timestamp_t &timestamp) const = 0;

        std::unique_ptr<SnapshotId> clone(BackendDriver const &backendDriver) const
        {
            return backendDriver.snapshotId(encoded());
        }

        /**
         * Equality operator.
         * Any two SnapshotIds are considered equal if and only if their encoded forms
         * are bytewise-equal.
         */
        bool operator==(SnapshotId const &other) const
        {
            return encoded() == other.encoded();
        }

        virtual ~SnapshotId() = default;
    };

    struct SnapshotDoesNotExist : std::runtime_error
    {
        SnapshotDoesNotExist(SnapshotId const &snapshotId)
            : std::runtime_error("Snapshot does not exist: " + snapshotId.pretty()),
              encodedSnapshotId(snapshotId.encoded())
        {}

        SnapshotDoesNotExist(SnapshotId const &snapshotId, std::string what)
            : std::runtime_error(std::move(what)),
              encodedSnapshotId(snapshotId.encoded())
        {}

        SnapshotId::Encoded encodedSnapshotId;
    };

    /**
     * Thrown if the given snapshot does exist, but is in some way invalid.
     */
    struct SnapshotInvalid : std::runtime_error
    {
        SnapshotInvalid(SnapshotId const &snapshotId)
            : std::runtime_error("Snapshot invalid: " + snapshotId.pretty()),
              encodedSnapshotId(snapshotId.encoded())
        {}

        SnapshotInvalid(SnapshotId const &snapshotId, std::string_view const what)
            : std::runtime_error(std::string("Snapshot invalid (")
                                 .append(snapshotId.pretty())
                                 .append("): ")
                                 .append(what)),
              encodedSnapshotId(snapshotId.encoded())
        {}

        SnapshotId::Encoded encodedSnapshotId;
    };

    /**
     * Take a snapshot and return a corresponding SnapshotId instance,
     * alongwith a boolean which, if true, indicates the snapshot is already
     * old and another should be taken.
     *
     * On drivers that support declaring snapshot identifiers (e.g., zfs, btrfs),
     * the identifier shall in some way include the given node and uuid.
     * Given that the uuid is a function of information known to the repository,
     * if a snapshot already exists with this same uuid, then this snapshot
     * is assumed to be a valid replacement to taking a new snapshot,
     * in which case a corresponding SnapshotId is returned along with a true
     * value for the boolean. The boolean is true because while the snapshot is
     * a valid replacement to taking a new snapshot from a structural point of view,
     * the snapshot may nevertheless be old, and so a new snapshot should be taken.
     *
     * Reasons for why a snapshot may already exist with the given uuid include:
     *
     * - an unfortunately timed powerloss,
     * - the repository directory being rolled back on the filesystem
     *   without a similar rollback of the dataset.
     *
     * On drivers that do not support declaring snapshot identifiers (e.g., git),
     * the snapshot cannot collide and the "forceful" discussion above is not
     * relevant.
     *
     * The resulting instance must only be used with the current BackendDriver instance.
     */
    virtual std::pair<std::unique_ptr<SnapshotId>, bool> snapshot(
        Node node,
        Uuid const &uuid) = 0;

    /**
     * Ensure the given snapshot exists and is valid, so far as the backend can check,
     * throwing SnapshotDoesNotExist or SnapshotInvalid
     * (both deriving from std::runtime_error) otherwise.
     *
     * Design notes: this throws rather than returning as the expectation
     * is that the snapshot
     * will exist; if it doesn't, it's because it's been deleted or corrupted external to
     * diffmonger.
     * Controlling logic should not use this to query whether a given snapshot exists;
     * it should know that the snapshot _should_ exist, using this function
     * purely to confirm that nothing outside its control has changed this.
     *
     *
     * TODO: It does not make sense to cache the verify() outcome,
     * given that remove_snapshot() would invalid that cached value.
     */
    virtual void verify(SnapshotId const &snapshotId) const = 0;
    virtual bool exists(SnapshotId const &snapshotId) const = 0;

    /**
     * Removes the given snapshot.
     */
    virtual void remove_snapshot(SnapshotId const &snapshotId) = 0;
    virtual void initial(SnapshotId const &snapshotId, FdOwner out) = 0;
    virtual void diff(SnapshotId const &fromSnapshotId,
                      SnapshotId const &toSnapshotId,
                      FdOwner out) = 0;
    virtual void restoreInitial(SnapshotId const &snapshotId, FdOwner in) = 0;
    virtual void restoreDiff(SnapshotId const &fromSnapshotId,
                             SnapshotId const &toSnapshotId,
                             FdOwner in) = 0;


    /**
     * Obtain an unverified SnapshotId instance from an encoded representation
     * that's previously been created by SnapshotId::encoded().
     * The resulting instance must only be used with
     * the BackendDriver that created the original instance.
     * The backend does not check that the snapshot still exists and is valid;
     * call verify() if that is needed.
     */
    std::unique_ptr<SnapshotId>
    virtual snapshotId(SnapshotId::Encoded const &encoded) const = 0;

    /**
     * String representing the version of the underlying backend driver.
     * For example, for zfs, this would be a combination of the following:
     *   - zfs --version
     *   - /sys/module/zfs/version
     *   - modinfo zfs
     */
    virtual std::string version() const = 0;

    virtual ~BackendDriver() = default;

    struct Factory
    {
        virtual std::unique_ptr<BackendDriver> create() const = 0;
        virtual ~Factory() = default;
    };
};

}

#endif
