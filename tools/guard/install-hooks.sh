#!/bin/sh
# Aktiviert die Git-Hooks des Guards für dieses Repo.
set -e
cd "$(git rev-parse --show-toplevel)"
git config core.hooksPath tools/guard/hooks
echo "guard: Hooks aktiv (core.hooksPath = tools/guard/hooks)"
