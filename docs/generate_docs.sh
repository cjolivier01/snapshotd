#!/usr/bin/env bash
set -euo pipefail

tool="auto"
source_root=""
output_root=""

usage() {
  cat <<'EOF'
usage: docs/generate_docs.sh [--tool auto|doxygen|clang-doc] [--source-root PATH] [--output-root PATH]

Generates documentation for snapshotd.
- Prefers Doxygen when available.
- Falls back to clang-doc + mkdocs when Doxygen is unavailable.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tool)
      tool="$2"
      shift 2
      ;;
    --source-root)
      source_root="$2"
      shift 2
      ;;
    --output-root)
      output_root="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="${source_root:-$(cd "$script_dir/.." && pwd)}"
output_root="${output_root:-$repo_root/build/docs}"
site_dir="$output_root/site"

have_tool() {
  command -v "$1" >/dev/null 2>&1
}

pick_tool() {
  case "$tool" in
    auto)
      if have_tool doxygen; then
        echo "doxygen"
        return 0
      fi
      if have_tool clang-doc && have_tool mkdocs; then
        echo "clang-doc"
        return 0
      fi
      echo "snapshotd docs generation requires either doxygen or both clang-doc and mkdocs on PATH." >&2
      exit 1
      ;;
    doxygen)
      have_tool doxygen || {
        echo "DOCS_TOOL=doxygen requested, but doxygen is not available on PATH." >&2
        exit 1
      }
      echo "doxygen"
      ;;
    clang-doc)
      if ! have_tool clang-doc || ! have_tool mkdocs; then
        echo "DOCS_TOOL=clang-doc requested, but clang-doc and mkdocs are both required on PATH." >&2
        exit 1
      fi
      echo "clang-doc"
      ;;
    *)
      echo "unsupported docs tool mode: $tool" >&2
      exit 1
      ;;
  esac
}

generate_with_doxygen() {
  local temp_doxy
  temp_doxy="$(mktemp)"
  trap 'rm -f "$temp_doxy"' RETURN

  cp "$repo_root/docs/Doxyfile" "$temp_doxy"
  cat >>"$temp_doxy" <<EOF
OUTPUT_DIRECTORY = $output_root
HTML_OUTPUT = site
INPUT = $repo_root/README.md $repo_root/docs $repo_root/src/csrc
IMAGE_PATH = $repo_root/docs
EOF

  rm -rf "$site_dir"
  mkdir -p "$output_root"
  echo "Generating docs with doxygen"
  doxygen "$temp_doxy"
}

generate_with_clang_doc_mkdocs() {
  local clang_output mkdocs_src mkdocs_config
  clang_output="$output_root/clang-doc-md"
  mkdocs_src="$output_root/mkdocs-src"
  mkdocs_config="$output_root/mkdocs.yml"

  rm -rf "$clang_output" "$mkdocs_src" "$site_dir"
  mkdir -p "$clang_output" "$mkdocs_src/api" "$mkdocs_src/design"

  mapfile -t source_files < <(find -L "$repo_root/src/csrc" -maxdepth 1 -type f \( -name '*.h' -o -name '*.cc' \) | sort)
  if [[ ${#source_files[@]} -eq 0 ]]; then
    echo "no C++ source files found under $repo_root/src/csrc" >&2
    exit 1
  fi

  echo "Generating API markdown with clang-doc"
  clang-doc \
    --doxygen \
    --format=md \
    --project-name=snapshotd \
    --output="$clang_output" \
    --extra-arg-before=-xc++ \
    --extra-arg-before=-std=c++17 \
    --extra-arg-before=-I"$repo_root" \
    "${source_files[@]}"

  cp -R "$clang_output/." "$mkdocs_src/api/"
  cp "$repo_root/README.md" "$mkdocs_src/overview.md"
  cp "$repo_root/docs/safe-root-criu-broker-design.md" \
    "$mkdocs_src/design/safe-root-criu-broker-design.md"

  cat >"$mkdocs_src/index.md" <<'EOF'
# snapshotd Documentation

This site was generated with the `clang-doc` plus `mkdocs` fallback path.

- [Overview](overview.md)
- [Design](design/safe-root-criu-broker-design.md)
- [API Reference](api/index.md)
EOF

  cat >"$mkdocs_config" <<EOF
site_name: snapshotd
docs_dir: $mkdocs_src
site_dir: $site_dir
use_directory_urls: false
nav:
  - Home: index.md
  - Overview: overview.md
  - Design: design/safe-root-criu-broker-design.md
  - API: api/index.md
EOF

  echo "Generating site with mkdocs"
  mkdocs build -q -f "$mkdocs_config"
}

selected_tool="$(pick_tool)"
case "$selected_tool" in
  doxygen)
    generate_with_doxygen
    ;;
  clang-doc)
    generate_with_clang_doc_mkdocs
    ;;
esac

if [[ ! -f "$site_dir/index.html" ]]; then
  echo "docs generation did not produce $site_dir/index.html" >&2
  exit 1
fi

echo "Documentation site ready at $site_dir/index.html"
