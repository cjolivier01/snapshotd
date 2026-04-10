BUILD_TARGETS := \
	//src/csrc:snapshotctl \
	//src/csrc:snapshotd \
	//src/csrc:snapshot-worker \
	//:snapshotd_deb
PACKAGE_NAME := snapshotd_0.1.0_amd64.deb
PACKAGE_BAZEL := bazel-bin/$(PACKAGE_NAME)
PACKAGE_TMP := /tmp/$(PACKAGE_NAME)
PACKAGE_LOCAL := $(CURDIR)/$(PACKAGE_NAME)
DOCS_SCRIPT := docs/generate_docs.sh
DOCS_OUTPUT_ROOT := build/docs
DOCS_HTML_INDEX := $(DOCS_OUTPUT_ROOT)/site/index.html

.DEFAULT_GOAL := help

.PHONY: help debug release deb docs install clean distclean

help:
	@printf "\n"
	@printf "%s\n" "snapshotd build targets"
	@printf "%s\n" "======================="
	@printf "\n"
	@printf "%s\n" "Build"
	@printf "%s\n" "-----"
	@printf "  %-12s %s\n" "debug" "Build snapshotd binaries and Debian package with Bazel debug settings (-c dbg)."
	@printf "  %-12s %s\n" "release" "Build snapshotd binaries and Debian package with Bazel optimized settings (-c opt)."
	@printf "  %-12s %s\n" "deb" "Build the release Debian package and copy it to the repository root."
	@printf "  %-12s %s\n" "docs" "Generate docs under build/docs/site using Doxygen or a clang-doc+mkdocs fallback."
	@printf "\n"
	@printf "%s\n" "Install"
	@printf "%s\n" "-------"
	@printf "  %-12s %s\n" "install" "Build the release Debian package, copy it to /tmp, and install it with apt."
	@printf "\n"
	@printf "%s\n" "Maintenance"
	@printf "%s\n" "-----------"
	@printf "  %-12s %s\n" "clean" "Remove Bazel output state with 'bazel clean'."
	@printf "  %-12s %s\n" "distclean" "Remove all Bazel state with 'bazel clean --expunge'."
	@printf "\n"
	@printf "%s\n" "Usage"
	@printf "%s\n" "-----"
	@printf "%s\n" "  make <target>"
	@printf "\n"

debug:
	bazel build -c dbg $(BUILD_TARGETS)

release:
	bazel build -c opt $(BUILD_TARGETS)

deb:
	bazel build -c opt //:snapshotd_deb
	install -m 0644 $(PACKAGE_BAZEL) $(PACKAGE_LOCAL)
	@printf "%s\n" "Copied Debian package to $(PACKAGE_LOCAL)"

docs:
	$(DOCS_SCRIPT) --source-root "$(CURDIR)" --output-root "$(CURDIR)/$(DOCS_OUTPUT_ROOT)"
	@printf "%s\n" "Generated HTML docs: $(DOCS_HTML_INDEX)"

install: release
	install -m 0644 $(PACKAGE_BAZEL) $(PACKAGE_TMP)
	if [ "$$(id -u)" -eq 0 ]; then \
		apt install --reinstall $(PACKAGE_TMP); \
	else \
		sudo apt install --reinstall $(PACKAGE_TMP); \
	fi

clean:
	bazel clean

distclean:
	bazel clean --expunge
