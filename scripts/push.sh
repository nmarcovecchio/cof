#!/usr/bin/env bash
# Push this repo to GitHub (Linux / macOS). From the repo root: ./scripts/push.sh
set -euo pipefail
cd "$(dirname "$0")/.."

git status
printf "Commit message: "
read -r msg
if [ -z "${msg}" ]; then
  echo "Empty message, abort."
  exit 1
fi

git add -A
git status --short
printf "Commit and push to origin? [y/N] "
read -r ok
case "${ok}" in
  y|Y|yes|YES) ;;
  *) echo "Aborted."; exit 0 ;;
esac

git commit -m "${msg}"
git push -u origin HEAD
echo "Done."
