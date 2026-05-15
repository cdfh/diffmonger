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
#include <stdexcept>

namespace diffmonger::serialisation
{

template <typename IntType>
[[nodiscard]] std::array<std::byte, sizeof(IntType)> to_bytes(IntType value)
{
    using UnsignedType = std::make_unsigned_t<IntType>;

    static_assert(std::is_integral_v<IntType>);
    static_assert(std::numeric_limits<UnsignedType>::is_modulo);

    UnsignedType const uvalue = static_cast<UnsignedType>(value);

    alignas(alignof(IntType)) std::array<std::byte, sizeof(IntType)> out;

    for (size_t i = 0; i != sizeof(IntType); ++i)
        out[i] = static_cast<std::byte>(static_cast<uint8_t>(uvalue >> (i * 8)));

    return out;
}

template <>
[[nodiscard]] inline std::array<std::byte, 1> to_bytes(std::byte const value)
{
    return { value };
}

template <typename IntType>
[[nodiscard]] IntType from_bytes(std::span<std::byte const, sizeof(IntType)> const bytes)
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
[[nodiscard]] inline std::byte from_bytes(std::span<std::byte const, 1> const bytes)
{
    return bytes[0];
}


class Serialiser;
class Deserialiser;

template <typename T>
struct Codec;

class Serialiser
{
public:
    explicit Serialiser(std::vector<std::byte> &buffer)
        : buffer(buffer)
    {}

    template <typename T>
    Serialiser &serialise(T const &value)
    {
        Codec<T>::serialise(*this, value);
        return *this;
    }

    void write_bytes(std::span<std::byte const> bytes)
    {
        buffer.insert(buffer.end(), bytes.begin(), bytes.end());
    }

private:
    std::vector<std::byte> &buffer;
};


class Deserialiser
{
public:
    explicit Deserialiser(std::span<std::byte const> data)
        : ptr(data.data()), end(data.data() + data.size())
    {}

    template <typename T>
    T deserialise()
    {
        return Codec<T>::deserialise(*this);
    }

    template <typename T>
    Deserialiser &deserialise(T &out)
    {
        out = deserialise<T>();
        return *this;
    }

    std::span<std::byte const> consume(size_t const nbytes)
    {
        assert(end >= ptr);

        if (static_cast<size_t>(end - ptr) < nbytes)
            throw std::runtime_error("Insufficient data (corrupt stream)");

        std::byte const *start = ptr;
        ptr += nbytes;

        return { start, nbytes };
    }

private:
    std::byte const *ptr;
    std::byte const *end;
};


template <typename T>
struct Codec
{
    static void serialise(Serialiser &, T const &)
    {
        static_assert(sizeof(T) == 0,
            "No Codec<T>::serialise defined for this type");
    }

    static T deserialise(Deserialiser &)
    {
        static_assert(sizeof(T) == 0,
            "No Codec<T>::deserialise defined for this type");
    }
};


template <std::integral T>
struct Codec<T>
{
    static void serialise(Serialiser &s, T const &value)
    {
        auto const bytes = to_bytes(value);
        s.write_bytes(bytes);
    }

    static T deserialise(Deserialiser &d)
    {
        auto const span = d.consume(sizeof(T));

        return from_bytes<T>(
            std::span<std::byte const, sizeof(T)>(
                span.data(), sizeof(T)));
    }
};


template <>
struct Codec<std::byte>
{
    static void serialise(Serialiser &s, std::byte const &b)
    {
        s.write_bytes(std::span{ &b, 1 });
    }

    static std::byte deserialise(Deserialiser &d)
    {
        auto const span = d.consume(1);
        return span[0];
    }
};


template <>
struct Codec<std::string>
{
    static void serialise(Serialiser &s, std::string const &str)
    {
        // length prefix
        s.serialise(str.size());

        s.write_bytes(
            std::as_bytes(std::span(str.data(), str.size())));
    }

    static std::string deserialise(Deserialiser &d)
    {
        size_t n = d.deserialise<size_t>();

        auto bytes = d.consume(n);

        return std::string(
            reinterpret_cast<char const *>(bytes.data()),
            bytes.size());
    }
};


template <typename T>
struct Codec<std::vector<T>>
{
    static void serialise(Serialiser &s, std::vector<T> const &v)
    {
        s.serialise(v.size());
        for (auto const &x: v)
            s.serialise(x);
    }

    static std::vector<T> deserialise(Deserialiser &d)
    {
        size_t n = d.deserialise<size_t>();

        std::vector<T> v;
        v.reserve(n);

        for (size_t i = 0; i != n; ++i)
            v.push_back(d.deserialise<T>());

        return v;
    }
};


template <typename T, size_t N>
struct Codec<std::array<T, N>>
{
    static void serialise(Serialiser &s, std::array<T, N> const &a)
    {
        for (auto const &x: a)
            s.serialise(x);
    }

    static std::array<T, N> deserialise(Deserialiser &d)
    {
        std::array<T, N> a{};

        for (auto &x : a)
            x = d.deserialise<T>();

        return a;
    }
};


template <typename T>
struct Codec<std::span<T>>
{
    static void serialise(Serialiser &s, std::span<T const> sp)
    {
        s.serialise(sp.size());
        for (auto const &x: sp)
            s.serialise(x);
    }

    static std::span<T> deserialise(Deserialiser &)
    {
        static_assert(sizeof(T) == 0,
            "Deserialising std::span is not supported (no ownership)");
    }
};

} // namespace diffmonger::serialisation

#endif
