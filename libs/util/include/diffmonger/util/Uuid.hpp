#ifndef DIFFMONGER_UTIL_UUID_HPP
#define DIFFMONGER_UTIL_UUID_HPP

#include <diffmonger/util/array.hpp>

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <span>

namespace diffmonger {

class Uuid
{
public:
    static constexpr size_t size = 16;

    using array_type = std::array<std::byte, size>;

    Uuid() = default;
    Uuid(Uuid const &) = default;
    Uuid(Uuid &&) = default;
    Uuid &operator=(Uuid const &) = default;
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

private:
    array_type bytes;
};

}

#endif
