#!/usr/bin/env bash
{

set -eo pipefail

# --------------------------
# Basic utilities
# --------------------------
warn() { printf "%s\n" "$*" >&2; }
die()  { warn "$@"; exit 1; }

usage() {
    die "DIFFMONGER_CMD=path DIFFMONGER_DATASET=dataset SNAPSHOT_COUNT=n $0"
}

# --------------------------
# Required environment checks
# --------------------------
[[ -n "$DIFFMONGER_CMD" && -x "$DIFFMONGER_CMD" ]] || usage
[[ -n "$DIFFMONGER_DATASET" ]] || usage

[[ "$SNAPSHOT_COUNT" =~ ^[0-9]+$ && "$SNAPSHOT_COUNT" -gt 0 ]] \
    || die "SNAPSHOT_COUNT must be a positive integer"

[[ -z "$NITERATIONS" ]] && NITERATIONS=100

DIFFMONGER_PASSPHRASE="$( uuidgen )"

printf "Using random test password: %s\n" "$DIFFMONGER_PASSPHRASE" >&2

# --------------------------
# Repositories
# --------------------------
primary_repository="$( mktemp -d "$USER-diffmonger.XXXXXXXXXXXX" )"
secondary_repository="$( mktemp -d "$USER-diffmonger.XXXXXXXXXXXX" )"

[[ -n "$primary_repository" && -n "$secondary_repository" ]] \
    || die "Could not create temp repositories"

# --------------------------
# Helpers
# --------------------------
diffmonger() {
    printf ': diffmonger %s\n' "$*" >&2
    if [[ -n "$DIFFMONGER_DEBUG" ]]; then
        gdb -ex run -ex bt -ex quit --args "$DIFFMONGER_CMD" "$@" || die "cmd failed: $*"
    else
        "$DIFFMONGER_CMD" "$@" || die "cmd failed: diffmonger $*"
    fi
}

echo_passphrase() {
    [[ -n "$DIFFMONGER_PASSPHRASE" ]] && printf "%s" "$DIFFMONGER_PASSPHRASE" || :
}

latest_guid() {
    zfs list -H -t snapshot -o guid "$1" | tail -n1
}

latest_node_number() {
    repository="$1"
    [[ -n "$repository" ]] || die "latest_node_number: no arg given"
    { printf -- "%s\n" "-1"
      find "$repository/snapshots" -type f -path '*/blob' \
          | sed -E 's#.*/snapshots/([0-9]+)/blob$#\1#'
    } | sort -n | tail -n1
}


# --------------------------
# Main Test
# --------------------------
inner_test() {
    # --------------------------
    # Initialize primary repository
    # --------------------------
    if [[ -z "$DIFFMONGER_PASSPHRASE" ]]; then
        diffmonger R="$primary_repository" init encryption=false zfs
    else
        diffmonger R="$primary_repository" init \
                   encryption=true zfs
        diffmonger R="$primary_repository" init-keypair \
                   additional-passphrase-fd=file://<( echo_passphrase ) \
                   no-passphrase=true
    fi

    diffmonger R="$primary_repository" show-init

    # --------------------------
    # Pick a random alive node
    # --------------------------
    stored_node="$( diffmonger alive node="$(( SNAPSHOT_COUNT - 1 ))" | shuf -n1 )"
    [[ -n "$stored_node" ]] || die "Failed to pick an alive node"
    stored_guid=""

    # --------------------------
    # Prepare dataset
    # --------------------------
    zfs get guid "$DIFFMONGER_DATASET/diffmonger-temporary-dataset" \
        >/dev/null 2>/dev/null && \
        zfs destroy -R "$DIFFMONGER_DATASET/diffmonger-temporary-dataset" || :
    zfs create -u "$DIFFMONGER_DATASET/diffmonger-temporary-dataset"

    # --------------------------
    # Snapshot creation
    # --------------------------
    for ((i=0; i != SNAPSHOT_COUNT; i++)); do
        diffmonger R="$primary_repository" snapshot \
                   "dataset=$DIFFMONGER_DATASET/diffmonger-temporary-dataset"

        if (( i == stored_node )); then
            stored_guid="$( latest_guid "$DIFFMONGER_DATASET/diffmonger-temporary-dataset" )"
            [[ -n "$stored_guid" ]] || die "Failed to capture GUID at node $stored_node"
        fi
    done

    latest_snapshot="$( latest_guid "$DIFFMONGER_DATASET/diffmonger-temporary-dataset" )"

    zfs destroy -R "$DIFFMONGER_DATASET/diffmonger-temporary-dataset"

    [[ -n "$stored_guid" ]] || \
        die "Did not store a GUID; should have stored node $stored_node"

    # --------------------------
    # Secondary repository: import primary repository
    # --------------------------
    diffmonger R="$secondary_repository" init encryption=false zfs
    diffmonger R="$primary_repository" export-repository \
               cmd="$DIFFMONGER_CMD" \
                 =R="$( realpath "$secondary_repository" )" \
                 =snapshot-import \
                 =create-snapshot=false \
                 =dataset="$DIFFMONGER_DATASET/diffmonger-temporary-dataset" \
               no-passphrase=true \
               additional-passphrase-fd=file://<( echo_passphrase ) \
               dataset="$DIFFMONGER_DATASET/diffmonger-temporary-dataset"

    # Compare repository snapshot dirs
    diff <( cd "$primary_repository" && find snapshots | sort ) \
         <( cd "$secondary_repository" && find snapshots | sort ) || \
        die "Snapshot dirs differ"

    diffmonger R="$primary_repository" prune-repository
    diffmonger R="$secondary_repository" prune-dataset \
               dataset="$DIFFMONGER_DATASET/diffmonger-temporary-dataset"

    rm -rf "$primary_repository"
    zfs destroy -R "$DIFFMONGER_DATASET/diffmonger-temporary-dataset"

    # --------------------------
    # Secondary repository: restore
    # --------------------------
    echo_passphrase | diffmonger R="$secondary_repository" restore \
                                 dataset="$DIFFMONGER_DATASET/diffmonger-temporary-dataset"

    [[ "$latest_snapshot" \
           = "$( latest_guid "$DIFFMONGER_DATASET/diffmonger-temporary-dataset" )" ]] \
        || die "latest snapshot differs"

    # --------------------------
    # Node-specific restore check
    # --------------------------
    zfs destroy -R "$DIFFMONGER_DATASET/diffmonger-temporary-dataset"

    echo_passphrase | diffmonger R="$secondary_repository" restore \
                                 dataset="$DIFFMONGER_DATASET/diffmonger-temporary-dataset" \
                                 node="$stored_node"

    restored_guid="$( latest_guid "$DIFFMONGER_DATASET/diffmonger-temporary-dataset" )"

    [[ "$restored_guid" = "$stored_guid" ]] \
        || die "node restore guid mismatch: expected $stored_guid got $restored_guid"

    # --------------------------
    # Cleanup
    # --------------------------
    rm -rf "$primary_repository" || :
    rm -rf "$secondary_repository" || :

    zfs destroy -R "$DIFFMONGER_DATASET/diffmonger-temporary-dataset"
}

for ((j=0; j != NITERATIONS; j++)); do
    inner_test
done

exit
}
