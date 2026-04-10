#!/usr/bin/env bash
set -euo pipefail

workspace_root="${TEST_SRCDIR}/${TEST_WORKSPACE}"

run_docs_check() {
  local mode="$1"
  local out_dir="$2"

  rm -rf "$out_dir"
  mkdir -p "$out_dir"
  make -C "$workspace_root" docs DOCS_TOOL="$mode" DOCS_OUTPUT_ROOT="$out_dir" \
    DOCS_HTML_INDEX="$out_dir/site/index.html" >/dev/null
  test -f "$out_dir/site/index.html"
  grep -qi "snapshotd" "$out_dir/site/index.html"
}

have_doxygen=0
have_fallback=0
temp_root="$(mktemp -d)"
trap 'rm -rf "$temp_root"' EXIT

if command -v doxygen >/dev/null 2>&1; then
  have_doxygen=1
fi

if command -v clang-doc >/dev/null 2>&1 && command -v mkdocs >/dev/null 2>&1; then
  have_fallback=1
fi

if [[ "$have_doxygen" -eq 0 && "$have_fallback" -eq 0 ]]; then
  echo "No documentation toolchain available; nothing to test."
  exit 0
fi

run_docs_check "auto" "$temp_root/auto"

if [[ "$have_doxygen" -eq 1 ]]; then
  run_docs_check "doxygen" "$temp_root/doxygen"
fi

if [[ "$have_fallback" -eq 1 ]]; then
  run_docs_check "clang-doc" "$temp_root/clang-doc"
  test -f "$temp_root/clang-doc/site/api/index.html"
  test -f "$temp_root/clang-doc/site/it-review-guide.html"
  test -f "$temp_root/clang-doc/site/design-reference.html"
fi
