#!/usr/bin/env bash
# Run PocketSync in this terminal (no install). Extra arguments are passed through, e.g. --token SECRET
cd "$(dirname "$0")" && exec python3 companion/pocketsync.py "$@"
