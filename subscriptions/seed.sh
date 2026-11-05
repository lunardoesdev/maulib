#!/bin/sh
set -eu

DB="${1:-data/subs.db}"
BIN="${MILIB_SUBS_BIN:-build/subscriptions/milib-subs}"
SEED="$(dirname "$0")/seed.txt"

mkdir -p "$(dirname "$DB")"

while read -r priority url; do
    case "$priority" in
        ''|\#*) continue ;;
    esac
    "$BIN" --db "$DB" --limit 0 sub add "$url" "$priority" >/dev/null
    echo "added $url (priority $priority)"
done < "$SEED"
