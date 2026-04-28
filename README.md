# Diffmonger

Diffmonger is an incremental backup program for Linux that supports taking
snapshots of a target dataset and restoring from these snapshots.
Each snapshot---other than the special case of the first snapshot---is
represented incrementally as a difference from a previous snapshot,
which itself is represented similarly, forming a chain of successive differences.
Diffmonger arranges snapshots into nodes of a Fenwick tree
and applies a pruning strategy that guarantees
 i) the length of the chain for the $n$-th taken snapshot shall be $O(\log n)$,
and ii) the total number of nodes in the tree at the time of taking the $n$-th
snapshot shall be $O(\log n)$.
Diffmonger's pruning strategy means that not all previous snapshots can be restored
at a given moment in time,
but it is guaranteed to always be possible to traverse backwards in history,
from the most recent snapshot to the first snapshot,
with a stride that starts at 1 and asymptotically doubles at each successive step.


<!-- Diffmonger arranges snapshots into nodes of a Fenwick tree, -->
<!-- which guarantees that the length of the chain of successive
differences required to restore
the $n$-th snapshot is logarithmic to $n$. -->
<!-- guaranteeing that the length of the chain for the $n$-th snapshot
shall be $O(\log n)$.
Furthermore,
diffmonger applies a pruning strategy that results in the total number of nodes
in the tree growing
logarithmically with respect to the number of snapshots taken,
yet that guarantees it is always possible to traverse backwards in history,
from any given snapshot to the initial snapshot,
with a stride that starts at 1 and asymptotically doubles at each successive step.
-->
<!--

Snapshots are represented as an chain of successive differences from the
repository's first snapshot.
Internally, each snapshot is represented as a node in a Fenwick tree data structure.
A property of Fenwick trees is that a traver
which is pruned upon each new
with the root corresponding to the first taken snapshot and each traversal from
the root
Each
The tree is pruned upon each new snapshot being taken so that
both depth and the total size of the tree grow logarithmically with the number of
taken snapshots.





The set of stored snapshots grows logarithmically with respect to the number of snapshots
taken. Upon each snapshot being taken, diffmonger selectively prunes the set of stored
snapshots so as to maintain this invariant while still ensuring each stored snapshot
is recoverable.


Diffmonger is an incremental backup program for Linux that allows reconstruction of
previous states, back to repository initiation, in exponentially increasing step sizes.


Diffmonger's two primary operations are `snapshot` and `restore`.
The `snapshot` operation creates a node in a tree
that represents the current
state of the _dataset_ (the thing being backed up).
The `restore` is given a node identifier (defaulting to the most recent node if none is given)
and restores the dataset to the state it occupied at the time of the corresponding snapshot.
Both the depth and the total number of elements in the tree are guaranteed
to grow logarithmically with increasing number of snapshots.
Despite these constraints,
diffmonger can restore to the nearby neighbourhood of any given node
with a resolution equal to the how many nodes in the past the target node is.
More formally,
if you wish to restore state as it was $n$ snapshots in the past,
then while you may not be able to restore to the exact $n$-th past snapshot,
you are guaranteed to be able to restore to _a_ snapshot in the discrete interval
$[n - n/3, n + n/3]$.
A consequence is that you shall always be able to restore to the previous node,
and the node before that.

-->

Diffmonger is agnostic to the dataset filesystem or format,
interfacing with the dataset via drivers.
At present, one one driver is implemented, zfs,
but it would be straightforward to add support for btrfs or git,
noting that the original motivating application for diffmonger's concept was to store
encrypted git repositories,
reconstructing a repository from a cascade of encrypted git bundles.

## Highlights

- First-class encryption support (Argon2id, XChaCha20-Poly1305) via libsodium
- Immutable repository format:
  files are never modified, with subsequent snapshot operations only adding new files.
  This makes diffmonger repositories well suited for cloud storage
- Offline pruning: encrypted repositories can be pruned of old nodes without requiring
  the passphrase.
- Ransomware-hardened pruning via quarantining:
  in use cases that demand it, new snapshots undergo quarantining, whereby they can still
  be used as normal, but they will not be considered when pruning.
  This is useful when the repository is stored on a separate, hardened
  server that only supports storing of new files and not deletion or modification
  of old files.
  The server would implement its own pruning by running diffmonger locally,
  rather than trusting deletion instructions from the client.
  To prevent the client from forcing deletions by spamming the repository with new snapshots,
  the server would place new snapshots in quarantine and refuse to prune old snapshots
  while the new snapshots are in quarantine.


## Status

At present, diffmonger is very much a work in progress.
It works for the author's purposes, but should not be assumed to be stable
or production ready.



# Notes

## Repository Corruption

Diffmonger does not provide tools for offline (i.e., without decryption)
detection of corruption to the repository.
This is intentional: there are existing widely available solutions for preventing
and/or detecting corruption at a memory level (e.g., ECC memory),
a filesystem level (e.g., zfs, btrfs),
and at a file transfer level (e.g., rsync),
and so the principal of separation of concern dictates
that these tools should be used instead of diffmonger reimplementing their functionality
locally.
