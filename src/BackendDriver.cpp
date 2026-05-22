#include "BackendDriver.hpp"

#include <diffmonger/util/Serialisation.hpp>

namespace diffmonger {

std::vector<std::byte> BackendDriver::SnapshotId::Encoded::serialise() const
{
    std::vector<std::byte> out;
    serialisation::Serialiser{out}
        .serialise(std::span(bytes))
        .serialise(node.value)
        .serialise(timestamp);
    return out;
}


BackendDriver::SnapshotId::Encoded
BackendDriver::SnapshotId::Encoded::deserialise(std::span<std::byte const> const bytes)
{
    std::vector<std::byte> encoded_bytes;
    Node node;
    timestamp_t timestamp;

    serialisation::Deserialiser{bytes}
        .deserialise(encoded_bytes)
        .deserialise(node.value)
        .deserialise(timestamp);

    return Encoded{std::move(encoded_bytes), node, timestamp};
}

}
