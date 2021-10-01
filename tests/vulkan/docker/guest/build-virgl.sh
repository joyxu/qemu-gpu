#!/bin/bash
# Build our virglrenderer

set -ex

git clone https://github.com/anholt/libepoxy
pushd /libepoxy
meson build -Dprefix=/usr/local
ninja -C build install
popd


pushd /virglrenderer

mkdir -p build/docker

if [ ! -f build/docker/build.ninja ]; then
    meson build/docker \
        -Dprefix=/usr/local \
        -Dplatforms=egl \
        -Dvenus-experimental=true \
        -Dminigbm_allocation=false \
        -Dbuildtype=debugoptimized
else
    meson --reconfigure build/docker \
        -Dprefix=/usr/local \
        -Dplatforms=egl \
        -Dvenus-experimental=true \
        -Dminigbm_allocation=false \
        -Dbuildtype=debugoptimized
fi

ninja -C build/docker install

popd
