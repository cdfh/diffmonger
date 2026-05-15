#include "Node.hpp"

#include <unordered_set>
#include <cstdlib>
#include <stdexcept>

using namespace diffmonger;

// These tests pass for n=2^28.

class WasSeen
{
public:
    WasSeen(Node const target) : target(target) {}
    operator bool() const { return result; }

    void operator()(Node const node)
    {
        result |= target == node;
    }
private:
    Node target;
    size_t result = 0;
};

bool was_pruned(Node const node, Node const doomed)
{
    size_t seen = false;
    node.prune(
        [&] (Node const victim)
        { seen |= victim == doomed; });
    return seen;
}


void run(size_t const n)
{
    Node cur;

    auto const fassert =
        [&cur] (bool b, char const *str)
        {
            if (!b)
                throw std::logic_error(
                    std::string(str).append(": ").append(std::to_string(cur.value)));
        };

    std::unordered_set<size_t> aliveset = { 0 };
    aliveset.reserve(256);
    size_t longdead_j = 0, veryalive_j = 0;
    Node const end{n + 1};
    for (cur = Node{1}; cur != end; cur = cur.next())
    {
        auto const demise = cur.time_of_demise();

        {
            WasSeen wasPruned{cur};
            demise.prune(wasPruned);
            fassert(wasPruned, "wasPruned");
        }

        if (demise.value > 0)
        {
            Node const still_alive{demise.value - 1};
            WasSeen wasAlive{cur};
            still_alive.alive_after(wasAlive);
            fassert(wasAlive, "wasAlive");
        }

        {
            WasSeen wasAlive{cur};
            demise.alive_after(wasAlive);
            fassert(!wasAlive, "!wasAlive");
        }

        {
            Node const longdead{demise.value + longdead_j};
            WasSeen wasAlive{cur};
            longdead.alive_after(wasAlive);
            fassert(!wasAlive, "!wasAlive (longdead)");

            if (++longdead_j > 4*cur.value)
                longdead_j = 0;
        }

        {
            if (++veryalive_j >= (demise.value - cur.value))
                veryalive_j = 0;

            Node const veryalive{cur.value + veryalive_j};
            WasSeen wasAlive{cur};
            veryalive.alive_after(wasAlive);
            fassert(wasAlive, "wasAlive (veryalive)");
        }

        {
            // The parent of the next node must always be in the current fenwick path.
            WasSeen wasInPath{cur.next().fenwick_parent()};
            cur.fenwick_path_inc_root(wasInPath);
            fassert(wasInPath, "wasInPath");
        }

        {
            auto const [it, b] = aliveset.emplace(cur.value);
            fassert(b, "Already in set");
            cur.prune(
                [&] (Node const pruned)
                {
                    bool const wasErased = aliveset.erase(pruned.value);
                    fassert(wasErased, "Victim not present in set");
                });
            size_t count = 0;
            cur.alive_after(
                [&] (Node const alive)
                {
                    fassert(aliveset.contains(alive.value), "Not present in set");
                    ++count;
                });
            fassert(count == aliveset.size(), "Set wrong size");
            fassert(count < 256, "Set way too big");

            for (auto const node: aliveset)
                fassert(aliveset.contains(Node{node}.fenwick_parent().value),
                        "Set does not contain parent");
        }

        {
            Node prev = cur; // <-- Monotonically decreases.
            cur.alive_after(
                [&] (Node const alive)
                {
                    if (alive == cur)
                        return;

                    /* The nodes that are unreachable (i.e., because they're not alive).
                     * Note: this is -1 as we're counting the number of /skipped/ integers
                     * in a consecutive sequence.
                     */
                    auto const unreachable_gap_width = prev.value - alive.value - 1;
                    auto const d = cur.value - alive.value;
                    fassert((alive.value < prev.value), "alive < prev");
                    fassert(2*unreachable_gap_width <= d, "gap width");
                    prev = alive;
                });
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr,
                "Call as: %s N\n", argv[0]);
        exit(1);
    }

    size_t const n = atoi(argv[1]);

    run(n);

    return 0;
}
