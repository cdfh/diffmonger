#include "Node.hpp"

#include <cstdio>
#include <cstdlib>

using namespace diffmonger;

template <typename T, typename U>
struct MaxAt
{
    T value;
    U at;

    MaxAt(T const initial_value, U initial_at)
        : value(initial_value),
          at(initial_at)
    {}

    void update(T const value, U const at)
    {
        if (value > this->value)
        {
            this->value = value;
            this->at = at;
        }
    }
};

void run(size_t const n)
{
    MaxAt max_disk_usage{0., Node{0}};
    MaxAt max_disk_writes{0., Node{0}};

    size_t disk_write_cost = 0;

    Node node{0};
    for (size_t i=0; i != n; ++i)
    {
        size_t disk_usage_cost = 0;

        node.alive_after(
            [&] (Node const alive)
            {
                disk_usage_cost += alive.value - alive.fenwick_parent().value;
            });

        node = node.next();


        size_t const diff = node.value - node.fenwick_parent().value;
        // = node - (node + (node & -node))
        // = node & -node

        disk_usage_cost += diff;
        max_disk_usage.update(static_cast<double>(disk_usage_cost + 1)/(node.value + 1), node);

        disk_write_cost += diff;
        max_disk_writes.update(static_cast<double>(disk_write_cost + 1)/(node.value + 1), node);
    }

    printf("Maximum relative cost of %f at Node %zu\n",
           max_disk_usage.value, max_disk_usage.at.value);


    /*
     * max_disk_writes(n) shall be O(log n).
     * Prove by showing disk_write_cost(n) is O(nlog n).
     * Proof:
     * Assume n = 2^k.
     * [This is without loss of generality:
     *  Suppose n != 2^k. Then n = 2^k + m, where m < 2^k.
     *  By assumption, disk_write_cost(n) is O(log n). Thus, also in O(log n) are:
     *    disk_write_cost(2^k + m) and disk_write_cost(2^(k + 1)). Since 2^(k + 1) > n,
     *    disk_write_cost(n) is also O(log n).]
     * disk_write_cost = sum_{i=0}^{2^k} LSB(i).
     * LSB(i) = 2^j if i = m*2^j, for odd m. Since i < 2^k, 1 <= m < 2^(k - j).
     * There are therefore 2^(k - j)/2 = 2^(k - j - 1) such values of m.
     * Thus, reexpressing the sum as a combination of count*2^j, for j in [0, k], gives
     * disk_write_cost = sum_{j=0}^k count*2^j = sum_{j=0}^k 2^(k - j -1)*2^j
     *                                         = sum_{j=0}^k 2^(k - 1)
     *                                         = (k + 1)*2^(k - 1)
     *                                         = (log(n) + 1)*(n/2),
     * which is O(nlog n).
     */
    printf("Maximum relative disk writes of %f at Node %zu\n",
           max_disk_writes.value, max_disk_writes.at.value);
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
