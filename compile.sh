#!/usr/bin/env bash

set -e

TOOLKIT_VER="7.1"

echo -e "Compiling modules..."

while read PLATFORM KVER; do
  [ -n "$1" -a "${PLATFORM}" != "$1" ] && continue
  DIR="${KVER:0:1}.x"
  [ ! -d "${PWD}/${DIR}" ] && continue
  mkdir -p "/tmp/${PLATFORM}-${KVER}"
  docker run -u `id -u` --rm -t -v "${PWD}/${DIR}":/input -v "/tmp/${PLATFORM}-${KVER}":/output \
    andatoshiki/shiki-compiler:${TOOLKIT_VER} compile-module ${PLATFORM}
  for M in `ls /tmp/${PLATFORM}-${KVER}`; do
    [ -f ~/src/pats/modules/${PLATFORM}/$M ] && \
      # original
      cp ~/src/pats/modules/${PLATFORM}/$M "${PWD}/../${PLATFORM}-${KVER}" || \
      # compiled
      cp /tmp/${PLATFORM}-${KVER}/$M "${PWD}/../${PLATFORM}-${KVER}"
  done
  rm -rf /tmp/${PLATFORM}-${KVER}
done < PLATFORMS
