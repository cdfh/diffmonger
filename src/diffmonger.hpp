#ifndef DIFFMONGER_HPP
#define DIFFMONGER_HPP

#include <cstdint>
#include <cassert>

namespace diffmonger {

/**
 * Calls the functor on the nodes of the Fenwick path from, but not including,
 * the root to the given node in decreasing order.
 */
template <typename F>
void fenwick_path_exc_root(uint64_t node, F &&f)
{
    for (; node; node -= node & -node)
        f(node);
}

template <typename F>
void fenwick_path_inc_root(uint64_t const node, F &&f)
{
    fenwick_path_exc_root(node, f);
    f(0);
}

/**
 * Returns the parent of the given node, or returns the given node itself if it
 * is already the root.
 */
inline uint64_t fenwick_parent(uint64_t const node)
{
    return node - (node & -node);
}

/**
 * Calls the given function on each node that should be alive just after the
 * creation of the given node.
 * The function is called in order, with the node value monotonically decreasing
 * from the given node, down to the 0 node.
 *
 * The nodes that will be enumerated are those of the alive set,
 *
 * $$A_n = \left\{k \leq n \: | \: k + 3 \operatorname{LSB}(k) \: > \: n\right\}.$$
 */
template <typename F>
void alive_after(uint64_t const node, F &&falive)
{
    // Identifies candidates c such that c <= n and T(c) > node,
    // where T(c) = time_of_demise(c) = c + 3*LSB(c).

    uint64_t uint_2powm = 1;  // 2^m
    // Invariants on cur:
    //   - cur = q*2^m, for some q and m as given by uint_2powm.
    //   - cur <= node
    //   - cur >= 0
    // This variable can be considered an "anchor".
    uint64_t cur = node;
    while (1)
    {
        // Proof of the correspondence of alive_after() and time_of_demise().
        //
        // Let node = q*2^m + r, r < 2^m.
        // Since cur = q*2^m, cur is therefore the greatest multiple of 2^m <= node
        // (the next multiple, (q + 1)*2^m is greater than node because r < 2^m).
        //
        // Since cur is a multiple of 2^m, LSB(cur) >= 2^m.
        //
        // T(cur) = cur + 3*LSB(cur) >= q*2^m + 3*2^m
        //                            = (q + 3)*2^m
        //
        // Therefore, cur is alive if (q + 3)*2^m > node.
        // This is trivially true by definition of node:
        //   node = q*2^m + r, r < 2^m.
        // Thus, cur is alive.
        //
        // We have
        //   candidate = cur + 2^(m - 2) = q*2^m + 2^(m - 2) = (q*4 + 1)*2^(m - 2),
        // which is alive if candidate <= node and T(candidate) > node.
        // The first condition (i.e., which checks whether it has been born yet)
        // is true iff (substituting in defs for candidate and node)
        //   q*2^m + 2^(m - 2) <= q*2^m + r
        //   <=> 2^(m - 2) <= r.
        // The second condition is as follows:
        //   T(candidate) > node
        //   <=> q*2^m + 2^(m - 2) + 3*LSB(q*2^m + 2^(m - 2)) > q*2^m + r
        //   <=> q*2^m + 2^(m - 2) + 3*2^(m - 2) > q*2^m + r
        //   <=> q*2^m + 4*2^(m - 2) > q*2^m + r
        //   <=> 4*2^(m - 2) > r
        //   <=> 2^m > r,
        // which is trivially true by definition of r.
        // Thus, candidate is alive if and only if
        //   2^(m - 2) <= r.
        // This is tested by the if statement below,
        // except that the if condition is stricter: it tests for
        //   r >= 2^(m - 2) + 2^(m - 1) = (3*2^m)/4,
        // noting that
        //   r = node - cur.
        // The reason for this stricter test is to eliminate overlap with cur
        // (i.e., calling falive() for both cur and candidate at different times).
        // It is thus also necessary to prove that falive() gets called for when
        //   2^(m - 2) <= r < 2^(m - 1).

        // The above can be seen by recognising that it becomes
        //   2^(m' - 1) <= r' < 2^m'
        // at the m' = m - 1 iteration of the loop (i.e., the previous iteration).
        // At this iteration, r' < 2^m' by definition. Again, the condition tested
        // in the if statement of the previous iteration is stricter:
        //   r >= (2^(m' - 1) + 2^(m' - 2)).
        // However, again we recognise the domain that overlaps with the previous
        // iteration. We thus follow induction until m' = 0
        // (i.e., corresponding to the first iteration).
        // At this point, 2^(m' - 2) = 0 (i.e., due to domain being of integers)
        // and the tested condition is no longer stricter than the condition
        // necessary to complete the proof.
        //
        // The complexity arises from the fact that at any point in time,
        // there is more than one fenwick path active:
        // the primary path (i.e., as fenwick_path() would report),
        // and one or more secondary paths that are kept for redundancy and so
        // that there is always an option of rolling-back to a recent restoration point.
        // From time to time, the paths overlap, hence the need for the stricter
        // condition to eliminate the overlap.

        assert(node >= cur);
        if ((node - cur) >= ((uint_2powm >> 1) + (uint_2powm >> 2)))
        {
            uint64_t const candidate = cur + (uint_2powm >> 2);
            // candidate = q*2^m + 2^(m - 2) = (4q + 1)*2^(m - 2)
            if (candidate != cur)
                falive(candidate);
        }
        falive(cur);

        if (cur == 0)
            break;

        // Maintains cur's invariant.
        // Further, successive iterations shall visit every fenwick_parent() of cur.
        // Proof:
        // Observe that (cur - 1) clears LSB(cur) and sets all bits to the right of it,
        // leaving all bits to the left unchanged. If cur = (2q + 1)*2^m, then
        // LSB(cur) = 2^m. In this case, the subsequent masking of (cur - 1) shall
        // clear all bits to the right of LSB(cur), so that the result is equal
        // to cur - LSB(cur). This is exactly the fenwick_parent().
        //
        // On the other hand, if cur cannot be expressed as (2q + 1)*2^m, then it needs
        // to be shown that eventually a subsequent iteration shall have
        //   cur' = cur - LSB(cur)
        // (i.e., where cur' is the value of cur at the time of the future iteration).
        //
        // This can be seen by noting that
        //   (cur - 1)&~(uint_2powm - 1)
        // 1. leaves the bits to the left of LSB(cur) unmodified,
        // 2. at each iteration, that iteration's cur value can be expressed as q*2^m, and
        // 3. m increases by 1 at each iteration.
        //
        // For any 2q*2^m (q, m >= 0), there exists an m' >= m and a q' >= 0 such that
        //   q*2^m = (2q' + 1)*2^m'.
        // Therefore, eventually m' shall be equal to m and the loop iteration shall
        // have visited the fenwick_parent() of the current iteration.
        //
        // Visual note: for Fenwick node n, the node n - 1 is the bottom-most descendent of
        // n's left sibling, or n's parent if n has no left sibling.
        // Successive applications of (- 1) shall either continue to visit descendents of
        // left siblings (but always maintaining the current parent as an ancestor),
        // or shall eventually traverse upwards, at which point the traversal shall
        // eventually visit the current fenwick_parent().
        cur = (cur - 1)&~(uint_2powm - 1);

        uint_2powm <<= 1;
    }
}

/**
 * The node shall die upon (i.e., just before) the creation of the returned node.
 * That is, the given node shall exist in alive_after(node - 1, ...),
 * but shall not exist in alive_after(node, ...).
 * Further, prune(node, ...) shall include time_of_demise(node).
 */
inline uint64_t time_of_demise(uint64_t const node)
{
    // Note:
    // Let node = (2q + 1)*2^(m - 1).
    // Then T(node) = time_of_demise(node) = node + 3*LSB(node)
    //              = (2q + 1)*2^(m - 1) + 3*LSB((2q + 1)*2^(m - 1))
    //              = (2q + 1)*2^(m - 1) + 3*2^(m - 1)
    //              = (2q + 4)*2^(m - 1)
    //              = (q + 2)*2^m
    // (noting that LSB((2q + 1)*2^(m - 1)) = 2^(m - 1) due to 2q + 1 being odd,
    // and thus LSB(2q + 1) = 1).
    // Thus, nodes always die at multiples of 2^m,
    // i.e., multiples of 2*LSB(node).
    //
    // Every node lasts exactly 1.5 times the distance to its
    // next natural power-of-two neighbour.
    //
    // Regarding the distribution of T(node):
    //
    // Let T(node) = time_of_demise(node) = node + 3*(node & -node)
    //                                    = node + 3*LSB(node)
    //                                    = node + h(node),
    //
    // Corollary 1:
    // LSB(n) = 2^k if and only if n mod 2^(k + 1) = 2^k.
    //
    // Theorem 1:
    // If N is a random variable uniformly distributed over
    // {0, 1, ..., 2^d - 1}, where d >= k + 1, then
    //   Pr(LSB(N) = 2^k) = 2^-(k + 1).
    //
    // Proof:
    // By Corollary 1,
    //   Pr(LSB(N) = 2^k) = Pr(N mod 2^(k + 1) = 2^k).
    // Since N is uniformly distributed over {0, 1, ..., 2^d - 1},
    // the random variable N mod 2^(k + 1) is uniformly distributed over
    // {0, 1, ..., 2^(k + 1) - 1}.
    // Therefore, Pr(N mod 2^(k + 1) = 2^k) = 2^-(k + 1).
    //
    // Corollary 2:
    //   Pr(h(N) = v) = Pr(3*LSB(N) = v)
    //    = { Pr(LSB(N) = 2^k) = 2^-(k + 1) if v = 3*2^k for some integer k >= 0,
    //        0 otherwise }.

    // i.e., node will survive until being pruned in 3*LSB(node) iterations.
    return node + 3*(node & -node);
}

// Identical to deriving process() from alive_after() for alive_after() as given below?
// Verified numerically for i up to 100,000,000
// (max_nalive was 52).

/**
 * Calls the given functor on each node that should be deleted just after the creation
 * of the given node.
 */
template <typename F>
void prune(uint64_t const node, F &&fdelete)
{
    // One way of interpreting this logic is that it is a solver for the equation
    //   node = j + 3*LSB(j).

    for (uint64_t mask = 1; node&(~mask); mask |= mask << 1)
    {   // node > mask

        // Break if LSB(node) <= mask.
        if (node&mask)
            break;

        // LSB(node) > mask

        // Note: mask != 0
        uint64_t const k = mask + (mask>>1) + 2; // = mask + 1 + ((mask + 1) >> 1);
                                               // = MSB(mask) | (MSB(mask) << 1)

        // node:     xxxxx10000...0000
        // k from:   00000000000000011
        //    (mask: 00000000000000001)
        //             ...
        // k until:  0000011000...0000
        //    (mask: 00000011111111111)

        if (node >= k)
        {
            // node:     xxxxx10000...0000  (at least one x being 1)
            // k from:   00000000000000011
            //    (mask: 00000000000000001)
            //             ...
            // k until:  0000011000...0000
            //    (mask: 00000011111111111)

            // Proving that time_of_demise(j) == node:
            //
            // Let m > 0 = lg(mask + 1), i.e., so that mask = 2^m - 1
            // LSB(x) = x & -x
            // T(x) time_of_demise(x) = x + 3*LSB(x)
            // k = 2^m + 2^(m - 1)
            //
            // Since LSB(node) > mask,
            // we have
            //   node = q*2^m,
            // for some integer q, where 2^m = mask + 1.
            // Note that
            //   k = 2^m + 2^(m + 1).
            // Then
            //   j = node - k = q*2^m - 2^m - 2^(m - 1)
            //                = (2q - 2 - 1)*2^(m - 1)
            //                = (2q - 3)*2^(m - 1).
            // Since node >= k, then the above is non-negative, and so 2q >= 3.
            // Substituting j into T(x) gives
            //   T(j) = j + 3*LSB(j).
            // Note that
            //   LSB(c*2^m) = LSB(c)*2^m for all non-negative integers c.
            // And so
            //   LSB(j) = LSB((2q - 3)*2^(m - 1)) = LSB(2q - 3)*2^(m - 1).
            // Further, note that 2q - 3 is odd, and so
            //   LSB(2q - 3) = 1.
            // Thus,
            //   LSB(j) = 2^(m - 1)
            // and so
            //   T(j) = (2q - 3)*2^(m - 1) + 3*2^(m - 1)
            //        = (2q - 3 + 3)*2^(m - 1)
            //        = q*2^m = node.
            // QED.

            uint64_t const j = node - k;
            fdelete(j);

            // Exhaustively tested up to 2^30 in the tests.
            assert(time_of_demise(j) == node);
        }
    }
}

}

#endif
