#ifndef DIFFMONGER_NODE_HPP
#define DIFFMONGER_NODE_HPP

#include "diffmonger.hpp"

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <limits>
#include <optional>
#include <stdexcept>

namespace diffmonger {

struct Node
{
    using value_t = uint64_t;
    value_t value;

    auto operator<=>(Node const &) const = default;
    //Node operator+(int64_t offset) const { return { value + offset }; }
    Node advance(value_t const n) const
    {
        if (n > std::numeric_limits<value_t>::max() - value)
            throw std::runtime_error("Node too big");
        return { value + n };
    }

    Node next() const
    {
        if (value == std::numeric_limits<decltype(value)>::max())
            throw std::runtime_error("Node too big");
        return { value + 1 };
    }

    /**
     * Calls the functor on the nodes of the Fenwick path from, but not including,
     * the root to the given node in decreasing order.
     */
    template <typename F>
    void fenwick_path_exc_root_exc_root(F &&f) const
    {
        tree::fenwick_path_exc_root(
            value,
            [&f] (size_t const node) { f(Node{node}); });
    }

    template <typename F>
    void fenwick_path_inc_root(F &&f) const
    {
        tree::fenwick_path_inc_root(
            value,
            [&f] (size_t const node) { f(Node{node}); });
    }

    [[nodiscard]] Node fenwick_parent() const
    {
        return Node{tree::fenwick_parent(value)};
    }

    std::optional<Node> fenwick_maybe_parent() const
    {
        if (isRoot())
            return std::nullopt;
        return { fenwick_parent() };
    }

    bool isRoot() const { return value == initial_value().value; }


   /**
    * Calls the given function on each node that should be alive just after the
    * creation of the given node.
    * The function is called in order, with the node value monotonically decreasing
    * from the given node, down to the 0 node.
    */
    template <typename F>
    void alive_after(F &&falive) const
    {
        tree::alive_after(value, [&falive] (size_t const node) { falive(Node{node}); });
    }

    Node time_of_demise() const
    {
        return Node{tree::time_of_demise(value)};
    }

    template <typename F>
    void prune(F &&fprune) const
    {
        tree::prune(value, [&fprune] (size_t const node) { fprune(Node{node}); });
    }

    /**
     * The initial node value.
     * This is the only node that has a full image stored for it; all others are diffs.
     */
    static constexpr Node initial_value() { return { 0 }; }
};

}
#endif
