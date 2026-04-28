#ifndef DIFFMONGER_NODE_HPP
#define DIFFMONGER_NODE_HPP

#include "diffmonger.hpp"

#include <cstdint>
#include <cstddef>
#include <optional>
#include <cassert>

namespace diffmonger {

struct Node {
    uint64_t value;

    auto operator<=>(Node const &) const = default;
    Node operator+(size_t inc) const { return { value + inc }; }

    Node next() const { return { value + 1 }; }

    /**
     * Calls the functor on the nodes of the Fenwick path from, but not including,
     * the root to the given node in decreasing order.
     */
    template <typename F>
    void fenwick_path(F &&f) const
    {
        diffmonger::fenwick_path(
            value,
            [&f] (size_t const node) { f(Node{node}); });
    }

    template <typename F>
    void fenwick_path_inc_root(F &&f) const
    {
        diffmonger::fenwick_path_inc_root(
            value,
            [&f] (size_t const node) { f(Node{node}); });
    }

    [[nodiscard]] Node fenwick_parent() const
    {
        return Node{diffmonger::fenwick_parent(value)};
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
        diffmonger::alive_after(value, [&falive] (size_t const node) { falive(Node{node}); });
    }

    Node time_of_demise() const
    {
        return Node{diffmonger::time_of_demise(value)};
    }

    template <typename F>
    void prune(F &&fprune) const
    {
        diffmonger::prune(value, [&fprune] (size_t const node) { fprune(Node{node}); });
    }

    /**
     * The initial node value.
     * This is the only node that has a full image stored for it; all others are diffs.
     */
    static constexpr Node initial_value() { return { 0 }; }
};

}
#endif
