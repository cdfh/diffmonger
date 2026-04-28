#ifndef DIFFMONGER_UTIL_ARRAY_HPP
#define DIFFMONGER_UTIL_ARRAY_HPP

#include <array>
#include <tuple>
#include <span>
#include <utility>

namespace diffmonger {

/**
 * Construct an array by casting each argument to the given type.
 * This is handy for constructing data arrays of numbers.
 *
 * See also std::to_array.
 */
template <typename T, typename ...Args>
constexpr auto make_array_cast(Args &&...args)
{
    return std::array<T, sizeof...(Args)>{{ static_cast<T>(args)... }};
}

/**
 * Use static_cast() to element-wise cast an array to the given type U.
 */
template <typename U, typename T>
auto cast_array(T &&array)
{
    return std::apply(
        [] (auto &&...xs)
        {
            return std::array<U, sizeof...(xs)>{ static_cast<U>(xs)... };
        },
        std::forward<T>(array)
    );
}

template <typename U, typename T, size_t N>
std::array<U,  N> cast_static_span(std::span<T const, N> const xs)
{
    return
        [&] <std::size_t... I>
        (std::index_sequence<I...>)
        {
            return std::array<U, N>{ static_cast<U>(xs[I])... };
        }(std::make_index_sequence<N>{});
}

}

#endif
