#!/usr/bin/env bash
set -euo pipefail

workspace_root="${TEST_SRCDIR}/${TEST_WORKSPACE}"
live_path="${PATH:-/usr/bin:/bin}"
base_path="/usr/bin:/bin"
make_bin="$(command -v make)"

tool_script() {
  printf '%s/tests/csrc/fake_docs_tools/%s' "$workspace_root" "$1"
}

link_tools() {
  local dest_dir="$1"
  shift
  mkdir -p "$dest_dir"
  for tool_name in "$@"; do
    ln -sf "$(tool_script "$tool_name")" "$dest_dir/$tool_name"
  done
}

run_docs_check() {
  local mode="$1"
  local out_dir="$2"
  local path_value="$3"
  local expected_marker="$4"

  rm -rf "$out_dir"
  mkdir -p "$out_dir"
  env PATH="$path_value" "$make_bin" -C "$workspace_root" docs DOCS_TOOL="$mode" DOCS_OUTPUT_ROOT="$out_dir" \
    DOCS_HTML_INDEX="$out_dir/site/index.html" >/dev/null
  test -f "$out_dir/site/index.html"
  grep -qi "snapshotd" "$out_dir/site/index.html"
  if [[ -n "$expected_marker" ]]; then
    grep -qi "$expected_marker" "$out_dir/site/index.html"
  fi
}

run_docs_check_expect_fail() {
  local mode="$1"
  local path_value="$2"
  local expected_text="$3"
  local out_dir="$4"
  local log_file

  log_file="$out_dir/failure.log"
  rm -rf "$out_dir"
  mkdir -p "$out_dir"
  if env PATH="$path_value" "$make_bin" -C "$workspace_root" docs DOCS_TOOL="$mode" \
      DOCS_OUTPUT_ROOT="$out_dir" DOCS_HTML_INDEX="$out_dir/site/index.html" >"$log_file" 2>&1; then
    echo "make docs unexpectedly succeeded for DOCS_TOOL=$mode" >&2
    exit 1
  fi
  grep -q "$expected_text" "$log_file"
}

temp_root="$(mktemp -d)"
trap 'rm -rf "$temp_root"' EXIT

doxygen_bin="$temp_root/doxygen-bin"
fallback_bin="$temp_root/fallback-bin"
all_tools_bin="$temp_root/all-tools-bin"

link_tools "$doxygen_bin" doxygen
link_tools "$fallback_bin" clang-doc mkdocs
link_tools "$all_tools_bin" doxygen clang-doc mkdocs

run_docs_check "auto" "$temp_root/hermetic-auto-doxygen" "$all_tools_bin:$base_path" "fake doxygen"
test -f "$temp_root/hermetic-auto-doxygen/site/it-review-guide.html"
test -f "$temp_root/hermetic-auto-doxygen/site/design-reference.html"

run_docs_check "doxygen" "$temp_root/hermetic-doxygen" "$doxygen_bin:$base_path" "fake doxygen"
test -f "$temp_root/hermetic-doxygen/site/it-review-guide.html"
test -f "$temp_root/hermetic-doxygen/site/design-reference.html"

run_docs_check "auto" "$temp_root/hermetic-auto-fallback" "$fallback_bin:$base_path" "fake mkdocs"
test -f "$temp_root/hermetic-auto-fallback/site/api/index.html"
test -f "$temp_root/hermetic-auto-fallback/site/it-review-guide.html"
test -f "$temp_root/hermetic-auto-fallback/site/design-reference.html"

run_docs_check "clang-doc" "$temp_root/hermetic-clang-doc" "$fallback_bin:$base_path" "fake mkdocs"
test -f "$temp_root/hermetic-clang-doc/site/api/index.html"
test -f "$temp_root/hermetic-clang-doc/site/it-review-guide.html"
test -f "$temp_root/hermetic-clang-doc/site/design-reference.html"

run_docs_check_expect_fail "doxygen" "$fallback_bin:$base_path" \
  "DOCS_TOOL=doxygen requested" "$temp_root/fail-doxygen"
run_docs_check_expect_fail "clang-doc" "$doxygen_bin:$base_path" \
  "DOCS_TOOL=clang-doc requested" "$temp_root/fail-clang-doc"

if PATH="$live_path" command -v doxygen >/dev/null 2>&1; then
  run_docs_check "doxygen" "$temp_root/live-doxygen" "$live_path" ""
fi

if PATH="$live_path" command -v clang-doc >/dev/null 2>&1 && PATH="$live_path" command -v mkdocs >/dev/null 2>&1; then
  run_docs_check "clang-doc" "$temp_root/live-clang-doc" "$live_path" ""
  test -f "$temp_root/live-clang-doc/site/api/index.html"
  test -f "$temp_root/live-clang-doc/site/it-review-guide.html"
  test -f "$temp_root/live-clang-doc/site/design-reference.html"
fi
