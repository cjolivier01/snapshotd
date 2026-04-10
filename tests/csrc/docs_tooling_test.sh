#!/usr/bin/env bash
set -euo pipefail

workspace_root="${TEST_SRCDIR}/${TEST_WORKSPACE}"
docs_script="${workspace_root}/docs/generate_docs.sh"

if [[ ! -x "$docs_script" ]]; then
  echo "docs generator script is not executable: $docs_script" >&2
  exit 1
fi

run_docs_check() {
  local mode="$1"
  local out_dir="$2"

  rm -rf "$out_dir"
  mkdir -p "$out_dir"
  "$docs_script" --tool "$mode" --source-root "$workspace_root" --output-root "$out_dir"
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
fi
