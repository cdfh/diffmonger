#ifndef DIFFMONGER_UTIL_MISC_HPP
#define DIFFMONGER_UTIL_MISC_HPP

#include <optional>
#include <span>
#include <memory>
#include <type_traits>
#include <cassert>
#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace diffmonger {

template <template <typename> typename C, typename T>
std::optional<T *> maybe_back(C<T> const container)
{
    return container.empty() ? std::nullopt : std::optional{ &container.back() };
}

template <typename T>
std::optional<typename std::decay_t<T>::value_type> maybe_back_value(T &&container)
{
    return container.empty() ? std::nullopt : std::optional{ std::forward<T>(container).back() };
}

template <typename It>
std::optional<It> maybe_back_iterator(It begin, It end)
{
    if (begin == end)
        return std::nullopt;
    return std::optional{ std::prev(end) };
}

// template <typename T>
// std::optional<T const &> maybe_back(std::span<T const> const span)
// {
//     return span.empty() ? std::nullopt : std::optional{ span.back() };
// }



template <typename T>
T *propagate_const_ptr(std::unique_ptr<T> &x) { return x.get(); }
template <typename T>
T const *propagate_const_ptr(std::unique_ptr<T> const &x) { return x.get(); }
template <typename T>
T *propagate_const_ptr(std::unique_ptr<T> &&x) { assert(!x); return nullptr; }


template <typename ...Args>
std::string concat(Args && ...args)
{
    std::size_t const total_length = (0 + ... + std::string_view(args).size());
    std::string out;
    out.reserve(total_length);
    (out.append(std::string_view(args)), ...);
    return out;
}


inline bool equal(std::span<std::byte const> const a, std::span<std::byte const> const b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

}

#endif
