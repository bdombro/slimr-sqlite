# Changelog

## Unreleased

## v0.1.2 - 2026-11-17

- chore: align CI/CD, contributing docs, and release tooling (#1)
  * chore: guard release and document VCPKG_OVERLAY_PORTS
  * chore: adopt CI/CD templates and docs/contributing.md
  * docs: maintainer setup for release App, secrets, rulesets
  * docs: soft alignment marker and release-bot contributing docs
- ci: poll gh run watch every 30s in release
- docs: document developing against unreleased libs
- build: switch macOS to llvm@23 and static libc++
- ci: sync release speed-up and release-only deps from cpp-templates
- chore: sync setup action from cpp-templates
- chore: move registry URLs to slimr-down-now
- test: cover named-param all/pluckAll and nested savepoint rollback
- test: annotate moved-from handle check for clang-tidy
- test: cover iterateLimited, registerFunction, and backup
- test: cover lifecycle, pragmas, migrations, Tx, and cache errors
- test: cover SqlValue types, named params, and Glaze extraction
- chore: configure bug-finding clang-tidy checks
- docs: README end-user first, add docs/development.md, AGENTS.md and CHANGELOG
- build: align CMake, justfile, CI and scripts with library template
- chore: add clang-format config and .git-blame-ignore-revs
- chore: add initial vcpkg registry version
- feat: split slimr-sqlite from slimr-cpp
- style: clang-format
- feat: add July MCP HTTP and OAuth support
  Harden SQLite handling and document native macOS dependency setup.
- Release packages: slimr-bugsink 0.1.1, slimr-curl 0.1.1, slimr-encoding 0.1.1, slimr-json 0.1.1, slimr-mcp 0.1.1, slimr-proc 0.1.1, slimr-reflect 0.1.1, slimr-sqlite 0.1.1, slimr-test 0.1.1, slimr-text 0.1.1, slimr-time 0.1.1, slimr-ws 0.1.1, slimr-yaml 0.1.1
- Migrate independent packages to native CMake and versioned vcpkg publishing
  Keep fuzz execution opt-in until its runtime gate is verified.
- Create slimr-cpp library repository

- Internal: switch macOS builds to `llvm@23` with static libc++ and hidden exports.
- Internal: raise sqlite.hpp line coverage to 90.10%.
- Internal: align tooling with the cpp-templates library template.
- Split `slimr-sqlite` from `slimr-cpp` into its own vcpkg registry repository.
- Internal: move the repository to `slimr-down-now/sqlite`
