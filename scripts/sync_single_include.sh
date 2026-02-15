#!/usr/bin/env bash
# Concatenate individual headers into single_include/tape_book.hpp.
# Strips duplicate #pragma once and internal #include "tape_book/..." lines.
# Collapses runs of blank lines to one. Ensures exactly one blank line
# separates each file's content.
#
# Usage: ./scripts/sync_single_include.sh
#   (run from the repository root)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO_ROOT/include/tape_book"
OUT="$REPO_ROOT/single_include/tape_book.hpp"

# Header dependency order
HEADERS=(
  config.hpp
  types.hpp
  spill_buffer.hpp
  spill_pool.hpp
  tape.hpp
  book.hpp
  multi_book_pool.hpp
  tape_book.hpp
)

TMPFILE=$(mktemp)
trap 'rm -f "$TMPFILE"' EXIT

first_pragma=1
file_idx=0

for hdr in "${HEADERS[@]}"; do
  # Collect filtered lines for this file into an array
  raw=()
  while IFS= read -r line; do
    # Strip #pragma once (keep only the very first one)
    if [[ "$line" =~ ^[[:space:]]*\#pragma\ once ]]; then
      if (( first_pragma )); then
        raw+=("$line")
        first_pragma=0
      fi
      continue
    fi
    # Strip internal includes
    if [[ "$line" =~ ^[[:space:]]*\#include\ \"tape_book/ ]]; then
      continue
    fi
    raw+=("$line")
  done < "$SRC/$hdr"

  # Collapse consecutive blank lines into a single blank line
  lines=()
  prev_blank=0
  for line in "${raw[@]}"; do
    if [[ -z "$line" ]]; then
      if (( !prev_blank )); then
        lines+=("")
        prev_blank=1
      fi
    else
      lines+=("$line")
      prev_blank=0
    fi
  done

  # Trim leading blank lines from this file's output
  start=0
  while (( start < ${#lines[@]} )) && [[ -z "${lines[$start]}" ]]; do
    (( start++ )) || true
  done

  # Trim trailing blank lines from this file's output
  end=$(( ${#lines[@]} - 1 ))
  while (( end >= start )) && [[ -z "${lines[$end]}" ]]; do
    (( end-- )) || true
  done

  # Write to temp file with a blank-line separator between files
  if (( file_idx > 0 && start <= end )); then
    printf '\n' >> "$TMPFILE"
  fi

  for (( i = start; i <= end; i++ )); do
    printf '%s\n' "${lines[$i]}" >> "$TMPFILE"
  done

  (( file_idx++ )) || true
done

mv "$TMPFILE" "$OUT"
trap - EXIT

echo "single_include/tape_book.hpp synced."
