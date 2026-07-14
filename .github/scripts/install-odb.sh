#!/usr/bin/env bash
# Install ODB 2.5.0 compiler + libodb + libodb-mysql from Code Synthesis binary packages.
set -euo pipefail

ODB_VERSION="${ODB_VERSION:-2.5.0}"
# ubuntu24.04 for GitHub Actions; debian12 as a fallback for Debian-based images.
ODB_DISTRO="${ODB_DISTRO:-ubuntu24.04}"
ODB_ARCH="${ODB_ARCH:-amd64}"
BASE="https://www.codesynthesis.com/download/odb/${ODB_VERSION}"

if [[ "${ODB_DISTRO}" == ubuntu* ]]; then
  URL_PREFIX="${BASE}/ubuntu/${ODB_DISTRO}/x86_64"
  SUFFIX="${ODB_VERSION}-0~${ODB_DISTRO}_${ODB_ARCH}"
else
  URL_PREFIX="${BASE}/debian/${ODB_DISTRO}/x86_64"
  SUFFIX="${ODB_VERSION}-0~${ODB_DISTRO}_${ODB_ARCH}"
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT
cd "${tmpdir}"

packages=(
  "odb_${SUFFIX}.deb"
  "libodb_${SUFFIX}.deb"
  "libodb-dev_${SUFFIX}.deb"
  "libodb-mysql_${SUFFIX}.deb"
  "libodb-mysql-dev_${SUFFIX}.deb"
)

for pkg in "${packages[@]}"; do
  echo "Downloading ${pkg}"
  curl -fsSL -O "${URL_PREFIX}/${pkg}"
done

sudo apt-get update
# MySQL/MariaDB client headers required by libodb-mysql-dev.
sudo apt-get install -y --no-install-recommends ./odb_*.deb ./libodb_*.deb ./libodb-dev_*.deb \
  ./libodb-mysql_*.deb ./libodb-mysql-dev_*.deb \
  default-libmysqlclient-dev || \
sudo apt-get install -y --no-install-recommends ./odb_*.deb ./libodb_*.deb ./libodb-dev_*.deb \
  ./libodb-mysql_*.deb ./libodb-mysql-dev_*.deb \
  libmariadb-dev

odb --version
pkg-config --modversion libodb
pkg-config --modversion libodb-mysql
