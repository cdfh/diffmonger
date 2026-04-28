#ifndef DIFFMONGER_UTIL_SERIALISATION_HPP
#define DIFFMONGER_UTIL_SERIALISATION_HPP

#include <diffmonger/util/PasswordBuffer.hpp>
#include <diffmonger/util/array.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <span>
#include <vector>
#include <cassert>
#include <utility>


namespace diffmonger {

// Possibly replace with cereal?

// Perhaps prefer cpu_to_le32() from <asm/byteorder.h>, etc?
// (Or maybe htonl() and friends from <arpa/inet.h>.)
// Should be implemented with intrinsics...
// TOOD: Confirm whether the optimiser can optimise these functions away.
template <typename IntType>
[[nodiscard]] std::array<std::byte, sizeof(IntType)> serialise(IntType value)
{
    using UnsignedType = std::make_unsigned_t<IntType>;

    static_assert(std::is_integral_v<IntType>);
    static_assert(std::numeric_limits<UnsignedType>::is_modulo);

    UnsignedType const uvalue = static_cast<UnsignedType>(value);

    alignas(alignof(IntType)) std::array<std::byte, sizeof(IntType)> out;

    for (size_t i = 0; i != sizeof(IntType); ++i)
        out[i] = static_cast<std::byte>(static_cast<uint8_t>(uvalue >> (i*8)));

    return out;
}

template <>
[[nodiscard]] inline std::array<std::byte, 1> serialise(std::byte const value)
{
    return std::array<std::byte, 1>{ value };
}

template <typename IntType>
[[nodiscard]] IntType deserialise(std::span<std::byte const, sizeof(IntType)> const bytes)
{
    using UnsignedType = std::make_unsigned_t<IntType>;

    static_assert(std::is_integral_v<IntType>);
    static_assert(std::numeric_limits<UnsignedType>::is_modulo);

    UnsignedType uvalue = 0;

    for (size_t i = 0; i != sizeof(IntType); ++i)
        uvalue |= static_cast<UnsignedType>(bytes[i]) << (i * 8);

    return static_cast<IntType>(uvalue);
}

template <>
[[nodiscard]] inline std::byte deserialise(std::span<std::byte const, 1> const bytes)
{
    return bytes[0];
}

class Serialiser {
public:
    Serialiser(std::vector<std::byte> &buffer) : buffer(buffer) {}

    template <typename T>
    requires std::is_integral_v<T>
    Serialiser &serialise(T const &value)
    {
        auto const bytes = diffmonger::serialise(value);
        buffer.insert(buffer.end(), bytes.begin(), bytes.end());
        return *this;
    }

    template <typename T, size_t N>
    Serialiser &serialise(std::array<T, N> const &arr)
    {
        return serialise_noprefix(std::span(arr.begin(), arr.end()));
    }

    template <typename T>
    Serialiser &serialise(std::span<T const> const xs)
    {
        serialise(xs.size());
        return serialise_noprefix(xs);
    }

    Serialiser &serialise(std::string_view const str)
    {
        return serialise(std::as_bytes(std::span(str)));
    }

    Serialiser &serialise(std::string const &str)
    {
        return serialise(std::as_bytes(std::span(str)));
    }

    Serialiser &serialise(std::byte const byte)
    {
        buffer.push_back(byte);
        return *this;
    }
private:
    template <typename T>
    Serialiser &serialise_noprefix(std::span<T const> const xs)
    {
        for (auto const &x: xs)
            serialise(x);
        return *this;
    }

    std::vector<std::byte> &buffer;
};

class Deserialiser
{
public:
    Deserialiser(std::span<std::byte const> const data)
        : ptr(data.data()), end(data.data() + data.size())
    {
    }

    template <typename T, size_t N>
    Deserialiser &deserialise(std::array<T, N> &out)
    {
        for (auto &x: out)
            deserialise<T>(x);
        return *this;
    }

    template <typename T, size_t N>
    Deserialiser &deserialise(std::array<std::byte, N> &out)
    {
        return deserialise(std::span(out));
    }

    Deserialiser &deserialise(std::span<std::byte> const xs)
    {
        auto const source_ptr = consume(xs.size());
        memcpy(xs.data(), source_ptr, xs.size());
        return *this;
    }

    template <typename T>
    Deserialiser &deserialise(std::span<T> const out)
    {
        for (auto &x: out)
            deserialise(x);
        return *this;
    }

    template <typename T>
    Deserialiser &deserialise(std::vector<T> &xs)
    {
        size_t n;  // <-- Do not trust n, it could have been corrupted.
        deserialise(n);

        xs = std::vector<T>();

        for (size_t i=0; i != n; ++i)
        {
            auto &x = xs.emplace_back();
            deserialise(x);
        }

        return *this;
    }

    Deserialiser &deserialise(std::string &out)
    {
        std::vector<std::byte> tmp;
        deserialise(tmp);
        out = std::string(reinterpret_cast<char *>(tmp.data()), tmp.size());
        return *this;
    }

    template <typename T>
    Deserialiser &deserialise(T &out)
    {
        auto const source_ptr = consume(sizeof(T));
        out = diffmonger::deserialise<T>(
            std::span<std::byte const, sizeof(T)>(source_ptr, sizeof(T)));
        return *this;
    }
protected:
    std::byte const *consume(size_t const nbytes)
    {
        assert(end >= ptr);
        if (size_t(end - ptr) < nbytes)
            throw std::runtime_error("Could not read type from string: too few bytes "
                                     "(data corruption?)");
        return std::exchange(ptr, ptr + nbytes);
    }
private:
    std::byte const *ptr;
    std::byte const *end;
};

}

#endif
