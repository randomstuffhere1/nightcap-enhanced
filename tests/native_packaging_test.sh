#!/usr/bin/env bash
# Modified by vii from komaruworld/mocktail. See README "About this fork".
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

readonly ROOT="${1:?source root is required}"
readonly CMAKE_FILE="${ROOT}/CMakeLists.txt"
readonly PKGBUILD="${ROOT}/packaging/arch/PKGBUILD"
readonly AUR_STABLE_PKGBUILD="${ROOT}/packaging/aur/nightcap/PKGBUILD"
readonly AUR_STABLE_SRCINFO="${ROOT}/packaging/aur/nightcap/.SRCINFO"
readonly AUR_BIN_PKGBUILD="${ROOT}/packaging/aur/nightcap-bin/PKGBUILD"
readonly AUR_BIN_SRCINFO="${ROOT}/packaging/aur/nightcap-bin/.SRCINFO"
readonly AUR_PKGBUILD="${ROOT}/packaging/aur/nightcap-git/PKGBUILD"
readonly AUR_SRCINFO="${ROOT}/packaging/aur/nightcap-git/.SRCINFO"
readonly AUR_WORKFLOW="${ROOT}/.github/workflows/aur.yml"
readonly AUR_PUBLISH_WORKFLOW="${ROOT}/.github/workflows/aur-publish.yml"
readonly WORKFLOW="${ROOT}/.github/workflows/packages.yml"
readonly RELEASE_WORKFLOW="${ROOT}/.github/workflows/release.yml"
readonly BUILD_WORKFLOW="${ROOT}/.github/workflows/build-packages.yml"
readonly CI_WORKFLOW="${ROOT}/.github/workflows/ci.yml"
readonly CCACHE_ACTION="${ROOT}/.github/actions/ccache/action.yml"
readonly README="${ROOT}/README.md"
readonly STUB_CMAKE="${ROOT}/stubs/CMakeLists.txt"
readonly ANDROID_STUB="${ROOT}/stubs/libandroid_stub.cc"

Fail() {
  printf 'Native packaging test failed: %s\n' "$*" >&2
  exit 1
}

bash -n "${PKGBUILD}"
bash -n "${AUR_STABLE_PKGBUILD}"
bash -n "${AUR_BIN_PKGBUILD}"
bash -n "${AUR_PKGBUILD}"

grep -Fq 'include(CPack)' "${CMAKE_FILE}" ||
  Fail 'CMake does not enable CPack'
grep -Fq 'CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON' "${CMAKE_FILE}" ||
  Fail 'DEB packages do not derive shared-library dependencies'
grep -Fq 'CPACK_RPM_PACKAGE_AUTOREQPROV ON' "${CMAKE_FILE}" ||
  Fail 'RPM packages do not derive runtime requirements'
grep -Fq 'set(MOCKTAIL_PACKAGE_VERSION' "${CMAKE_FILE}" ||
  Fail 'native package version cannot be set from the release tag'
grep -Fq 'https://github.com/CoderDayton/nightcap' "${CMAKE_FILE}" ||
  Fail 'native packages point at the upstream project page'
grep -Fq 'pkgname=nightcap' "${PKGBUILD}" ||
  Fail 'Arch package name is not stable'
grep -Fq "provides=('mocktail')" "${PKGBUILD}" ||
  Fail 'Arch package does not provide the upstream package name'
grep -Fq "conflicts=('mocktail')" "${PKGBUILD}" ||
  Fail 'Arch package can be installed beside upstream Mocktail'
grep -Fq "'sdl3>=3.4'" "${PKGBUILD}" ||
  Fail 'Arch package does not enforce the SDL minimum'
grep -Fq "'libplacebo'" "${PKGBUILD}" ||
  Fail 'Arch package does not declare the graphics composition dependency'
grep -Fq "'rust'" "${PKGBUILD}" ||
  Fail 'Arch package does not declare the Rust toolchain the kernels need'
grep -Fq 'pkgname=nightcap-git' "${AUR_PKGBUILD}" ||
  Fail 'AUR VCS package name is not stable'
grep -Fq 'pkgname=nightcap' "${AUR_STABLE_PKGBUILD}" ||
  Fail 'AUR source package name is not stable'
grep -Fq '#tag=v${pkgver}' "${AUR_STABLE_PKGBUILD}" ||
  Fail 'AUR source package does not pin its Git tag'
grep -Fq "'vulkan-headers'" "${AUR_STABLE_PKGBUILD}" ||
  Fail 'AUR source package does not use the packaged Vulkan headers'
grep -Fq 'pkgbase = nightcap' "${AUR_STABLE_SRCINFO}" ||
  Fail 'AUR source package has no generated .SRCINFO metadata'
grep -Fq 'pkgname=nightcap-bin' "${AUR_BIN_PKGBUILD}" ||
  Fail 'AUR binary package name is not stable'
grep -Fq 'pkgbase = nightcap-bin' "${AUR_BIN_SRCINFO}" ||
  Fail 'AUR binary package has no generated .SRCINFO metadata'
grep -Fq -- \
  "'nightcap::git+https://github.com/CoderDayton/nightcap.git#branch=main'" \
  "${AUR_PKGBUILD}" ||
  Fail 'AUR package does not build from this project Git repository'
grep -Fq "provides=('mocktail' 'nightcap')" "${AUR_PKGBUILD}" ||
  Fail 'AUR VCS package does not provide the stable package name'
grep -Fq "'vulkan-headers'" "${AUR_PKGBUILD}" ||
  Fail 'AUR VCS package does not use the packaged Vulkan headers'
grep -Fq 'makedepends = rust' "${AUR_STABLE_SRCINFO}" ||
  Fail 'AUR source package does not declare the Rust toolchain the kernels need'
grep -Fq 'makedepends = rust' "${AUR_SRCINFO}" ||
  Fail 'AUR VCS package does not declare the Rust toolchain the kernels need'
for aur_pkgbuild in "${AUR_STABLE_PKGBUILD}" "${AUR_PKGBUILD}"; do
  if grep -Eq \
      'libjnivm::git\+|vulkan-headers::git\+|git submodule (init|update)' \
      "${aur_pkgbuild}"; then
    Fail "AUR package downloads build-only submodules: ${aur_pkgbuild}"
  fi
  grep -Fq -- '-DMOCKTAIL_ENABLE_UPSTREAM_JNIVM=OFF' "${aur_pkgbuild}" ||
    Fail "AUR package enables the unused upstream JNI test library: ${aur_pkgbuild}"
done
grep -Fq -- \
  '-DMOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST=/usr/share/mocktail/metadata/' \
  "${AUR_PKGBUILD}" ||
  Fail 'AUR package embeds a build-tree compatibility manifest path'
grep -Fq 'pkgbase = nightcap-git' "${AUR_SRCINFO}" ||
  Fail 'AUR package has no generated .SRCINFO metadata'
grep -Fq 'git describe' "${AUR_PKGBUILD}" ||
  Fail 'AUR VCS package does not version from the release tag'
grep -Fq 'git describe' "${AUR_WORKFLOW}" ||
  Fail 'AUR workflow does not version from the release tag'
for pkgbuild in "${PKGBUILD}" "${AUR_STABLE_PKGBUILD}" "${AUR_BIN_PKGBUILD}"; do
  grep -Fqx 'pkgver=0.4.0' "${pkgbuild}" ||
    Fail "package version does not match the release being prepared: ${pkgbuild}"
done

# aur.yml only checks the nightcap-git recipe. Publishing is aur-publish.yml.
for unexpected in \
    'aur.archlinux.org' \
    'AUR_SSH_PRIVATE_KEY' \
    'push origin'; do
  if grep -Fq -- "${unexpected}" "${AUR_WORKFLOW}"; then
    Fail "AUR workflow should not publish: ${unexpected}"
  fi
done

for expected in \
    'workflow_call:' \
    'workflow_dispatch:' \
    'for package in nightcap nightcap-bin' \
    '${archive}.sha256' \
    'makepkg --printsrcinfo' \
    'secrets.AUR_SSH_PRIVATE_KEY' \
    'ssh://aur@aur.archlinux.org/' \
    'aur.archlinux.org ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEuBKrPzbawxA/k2g6NcyV5jmqwJ2s+zpgZGZ7tpLIcN' \
    'StrictHostKeyChecking=yes'; do
  grep -Fq -- "${expected}" "${AUR_PUBLISH_WORKFLOW}" ||
    Fail "AUR publish workflow is missing: ${expected}"
done

# The publish workflow holds the AUR key. It runs for a release tag or by hand,
# never for a push or a pull request, and it leaves nightcap-git to its own
# maintainer.
for unexpected in \
    'pull_request' \
    'push:' \
    'nightcap-git' \
    'StrictHostKeyChecking=no' \
    'set -x'; do
  if grep -Fq -- "${unexpected}" "${AUR_PUBLISH_WORKFLOW}"; then
    Fail "AUR publish workflow should not contain: ${unexpected}"
  fi
done
grep -Fq 'LINKER:--no-as-needed' "${STUB_CMAKE}" ||
  Fail 'system shims can lose their host libc dependencies'
grep -Fq 'MOCKTAIL_MINIZIP_HAS_STREAM_TELL' "${STUB_CMAKE}" ||
  Fail 'CMake does not detect the minizip-ng offset API'
grep -Fq 'mz_stream_tell(unzGetStream(archive))' "${ANDROID_STUB}" ||
  Fail 'Android assets do not support the minizip-ng offset API'

# Every action is pinned to a commit, and a commit that does not exist stops
# the workflow before it builds anything. The pins are listed here so a wrong
# one fails the contracts job instead of a build.
readonly ACTION_PINS=(
  'actions/cache/restore@0057852bfaa89a56745cba8c7296529d2fc39830'
  'actions/cache/save@0057852bfaa89a56745cba8c7296529d2fc39830'
  'actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1'
  'actions/configure-pages@45bfe0192ca1faeb007ade9deae92b16b8254a0d'
  'actions/deploy-pages@cd2ce8fcbc39b97be8ca5fce6e763baed58fa128'
  'actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c'
  'actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a'
  'actions/upload-pages-artifact@fc324d3547104276b827a68afc52ff2a11cc49c9'
)
while read -r pinned; do
  found=0
  for pin in "${ACTION_PINS[@]}"; do
    if [[ "${pinned}" == "${pin}" ]]; then
      found=1
      break
    fi
  done
  if [[ "${found}" -ne 1 ]]; then
    Fail "action is pinned to an unknown commit: ${pinned}"
  fi
done < <(grep -rhoE 'uses: [a-zA-Z0-9_.-]+/[a-zA-Z0-9_./-]+@[a-f0-9]{40}' \
  "${ROOT}/.github" | sed 's/^uses: //' | sort -u)

test -f "${CCACHE_ACTION}" ||
  Fail 'the ccache action is missing'
for expected in \
    'actions/cache/restore' \
    'actions/cache/save'; do
  grep -Fq -- "${expected}" "${CCACHE_ACTION}" ||
    Fail "the ccache action is missing: ${expected}"
done
for workflow in "${CI_WORKFLOW}" "${BUILD_WORKFLOW}"; do
  for expected in \
      'uses: ./.github/actions/ccache' \
      'mode: restore' \
      'mode: save'; do
    grep -Fq -- "${expected}" "${workflow}" ||
      Fail "workflow does not share the ccache: ${workflow}: ${expected}"
  done
done
# The build workflow is reusable, so the permission that lets the ccache
# action drop the older caches comes from the workflow that calls it.
for workflow in "${CI_WORKFLOW}" "${WORKFLOW}" "${RELEASE_WORKFLOW}"; do
  grep -Fq -- 'actions: write' "${workflow}" ||
    Fail "workflow cannot drop the older caches: ${workflow}"
done

# The builds live in build-packages.yml, shared by the nightly and the tagged
# release. Every container has to install the Rust toolchain the texture
# kernels need, and the nightly adds the portable tarball and the aarch64
# packages on top of what a release builds.
for expected in \
    'workflow_call:' \
    'APPIMAGE_NAME=Nightcap-x86_64.AppImage' \
    '|nightly|$APPIMAGE_NAME.zsync' \
    'test "$("dist/$APPIMAGE_NAME" --appimage-updateinformation)" = "$UPINFO"' \
    'MOCKTAIL_ANDROID_BUILD_TOOLS_ROOT' \
    'commandlinetools-linux-16111833_latest.zip' \
    './scripts/package_portable.sh' \
    'Nightcap-x86_64-portable.tar.zst' \
    'makepkg --dir' \
    '-G DEB' \
    '-G RPM' \
    '-DMOCKTAIL_PACKAGE_NAME=nightcap' \
    '-DMOCKTAIL_PACKAGE_VERSION=' \
    'package_version.sh --nightly' \
    'ubuntu-26.04-arm'; do
  grep -Fq -- "${expected}" "${BUILD_WORKFLOW}" ||
    Fail "package build workflow is missing: ${expected}"
done

# One Rust declaration per package manager: pacman and the AppImage container
# say rust, dnf says rust, apt says rustc cargo. A missing one fails the
# contracts job rather than a nightly run, once the build gets that far.
for expected in \
    'rust ccache mold' \
    'ccache mold rust cargo' \
    'rustc cargo'; do
  grep -Fq -- "${expected}" "${BUILD_WORKFLOW}" ||
    Fail "package build workflow does not declare the Rust toolchain: ${expected}"
done

# The build workflow is reusable, so it never carries a trigger or a write
# permission of its own. Both come from the workflow that calls it.
for unexpected in \
    'push:' \
    'pull_request:' \
    'schedule:' \
    'workflow_dispatch:' \
    'contents: write'; do
  if grep -Fq -- "${unexpected}" "${BUILD_WORKFLOW}"; then
    Fail "package build workflow should not contain: ${unexpected}"
  fi
done

# The nightly runs on every push to main and on demand, then tags `nightly`
# rather than upstream's `continuous`, so the two channels cannot overwrite
# each other.
for expected in \
    'push:' \
    'branches:' \
    'workflow_dispatch:' \
    'uses: ./.github/workflows/build-packages.yml' \
    'channel: nightly' \
    'git tag --force "$TAG"' \
    'gh release upload "$TAG"' \
    'github-actions[bot]' \
    'Nightcap-x86_64.AppImage' \
    'Nightcap-x86_64.AppImage.zsync' \
    'Nightcap-x86_64-portable.tar.zst' \
    'nightcap-nightly-x86_64.pkg.tar.zst' \
    'nightcap-nightly_amd64.deb' \
    'nightcap-nightly_arm64.deb' \
    'nightcap-nightly-x86_64.rpm' \
    'nightcap-nightly-aarch64.rpm' \
    'SHA256SUMS'; do
  grep -Fq -- "${expected}" "${WORKFLOW}" ||
    Fail "nightly workflow is missing: ${expected}"
done

# The nightly runs on a push to main and on demand. GitHub disables the
# scheduled workflows of an inactive repository, so a cron trigger would not
# keep the prerelease fresh, and a pull request trigger would build every
# artifact for every proposed change.
for unexpected in \
    'schedule:' \
    'cron:' \
    'pull_request:'; do
  if grep -Fq -- "${unexpected}" "${WORKFLOW}"; then
    Fail "nightly workflow should not contain: ${unexpected}"
  fi
done

if grep -Fq -- 'push:' "${CI_WORKFLOW}"; then
  Fail 'the CI workflow should not run on a push'
fi
grep -Fq -- 'pull_request:' "${CI_WORKFLOW}" ||
  Fail 'the CI workflow does not run for a pull request'

for expected in \
    'uses: ./.github/workflows/build-packages.yml' \
    'channel: release' \
    'Nightcap-x86_64.AppImage' \
    'gh release upload' \
    'uses: ./.github/workflows/aur-publish.yml'; do
  grep -Fq -- "${expected}" "${RELEASE_WORKFLOW}" ||
    Fail "release workflow is missing: ${expected}"
done

for workflow in "${RELEASE_WORKFLOW}" "${WORKFLOW}" "${BUILD_WORKFLOW}"; do
  for unexpected in \
      'createrepo' \
      'reprepro' \
      'aur.archlinux.org'; do
    if grep -Fq -- "${unexpected}" "${workflow}"; then
      Fail "workflow should not publish to a repository: ${workflow}: ${unexpected}"
    fi
  done
done

VERSION_SCRIPT="$(realpath -- "${ROOT}/scripts/package_version.sh")"
readonly VERSION_SCRIPT
# shellcheck disable=SC2046
unset $(git rev-parse --local-env-vars)
version_repo="$(mktemp -d)"
trap 'rm -rf -- "${version_repo}"' EXIT
VersionGit() {
  git -C "${version_repo}" -c user.name=test -c user.email=test@example.com \
    -c commit.gpgsign=false -c tag.gpgsign=false "$@"
}
VersionIn() {
  (cd "${version_repo}" && GITHUB_REF_NAME= bash "${VERSION_SCRIPT}" "$@")
}
VersionGit init --quiet
VersionGit commit --quiet --allow-empty -m first
[[ "$(VersionIn --nightly)" == "0.0.0+r1.g$(VersionGit rev-parse --short=7 HEAD)" ]] ||
  Fail 'nightly version without a release tag does not count from 0.0.0'
VersionGit tag v0.3.4
VersionGit commit --quiet --allow-empty -m second
VersionGit commit --quiet --allow-empty -m third
VersionGit tag continuous
[[ "$(VersionIn --nightly)" == "0.3.4+r2.g$(VersionGit rev-parse --short=7 HEAD)" ]] ||
  Fail 'nightly version does not sort between the last release and the next'
[[ "$(VersionIn v0.4.0)" == 0.4.0 ]] ||
  Fail 'release tag does not give its own version'
[[ "$(VersionIn main)" == 0.0.0 ]] ||
  Fail 'a branch does not give the placeholder version'

grep -Fq 'Ubuntu 26.04+' "${README}" ||
  Fail 'README has no Ubuntu source dependency guide'
grep -Fq 'Arch Linux' "${README}" ||
  Fail 'README has no Arch source dependency guide'
grep -Fq 'Fedora 44+' "${README}" ||
  Fail 'README has no Fedora source dependency guide'

printf 'Native packaging contract test passed\n'
