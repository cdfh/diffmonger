#ifndef DIFFMONGER_UTIL_UUID_HPP
#define DIFFMONGER_UTIL_UUID_HPP

#include <diffmonger/util/array.hpp>
#include <diffmonger/util/Serialisation.hpp>

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <span>

namespace diffmonger {

class Uuid;

template <>
struct serialisation::Codec<Uuid>;

class Uuid
{
public:
    static constexpr size_t size = 16;

    using array_type = std::array<std::byte, size>;

    constexpr Uuid() = default;
    constexpr Uuid(Uuid const &) = default;
    Uuid(Uuid &&) = default;
    constexpr Uuid &operator=(Uuid const &) = default;
    Uuid &operator=(Uuid &&) = default;

    auto operator<=>(Uuid const &) const = default;

    constexpr Uuid(array_type const bytes)
        : bytes{bytes}
    {}

    constexpr Uuid(std::span<std::byte const, size> const bytes)
        : Uuid(cast_static_span<std::byte>(bytes))
    {}

    Uuid(std::span<uint8_t const, size> const bytes)
        : Uuid{cast_static_span<std::byte>(bytes)}
    {}

    array_type const &serialised() const { return bytes; }

    static Uuid deserialise(std::span<std::byte const, size> const bytes)
    { return Uuid(bytes); }

    // Format: "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", where each x is a hex char
    void ascii(std::string &out) const;
    std::string ascii() const { std::string out; ascii(out); return out; }

    /* To generate a static random Uuid, use:
     *   od -An -tx1 -N16 -w16 /dev/urandom                             \
     *     | perl -ne '@x = /([0-9a-f]{2})/g; print "{", join(",", map "0x$_", @x), "}\n"'
     */
    static Uuid random();

    friend serialisation::Codec<Uuid>;

private:
    array_type bytes;
};


template <>
struct serialisation::Codec<Uuid>
{
    static void serialise(Serialiser &serialiser, Uuid const &uuid)
    {
        serialiser.serialise(uuid.bytes);
    }

    static Uuid deserialise(Deserialiser &deserialiser)
    {
        Uuid out;
        deserialiser.deserialise(out.bytes);
        return out;
    }
};

}

#endif
