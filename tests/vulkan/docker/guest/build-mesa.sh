#!/bin/bash
# Build our mesa

set -ex

#apt remove -y libegl1-mesa libegl-mesa0 libegl1-mesa-dev

pushd /mesa

mkdir -p build/docker

if [ ! -f build/docker/build.ninja ]; then
    meson build/docker \
        -Dprefix=/usr/local \
        -Ddri-drivers=i965 \
        -Dgallium-drivers=swrast,virgl,iris \
        -Dbuildtype=debugoptimized \
        -Dllvm=enabled \
        -Dglx=dri \
        -Degl=enabled \
        -Degl-native-platforms=x11,drm,wayland \
        -Dgbm=enabled \
        -Dgallium-vdpau=disabled \
        -Dgallium-vs=disabled \
        -Dvulkan-drivers=swrast,intel,virtio-experimental \
        -Dvalgrind=disabled
else
    meson --reconfigure build/docker \
        -Dprefix=/usr/local \
        -Ddri-drivers=i965 \
        -Dgallium-drivers=swrast,virgl,iris \
        -Dbuildtype=debugoptimized \
        -Dllvm=enabled \
        -Dglx=dri \
        -Degl=enabled \
        -Degl-native-platforms=x11,drm,wayland \
        -Dgbm=enabled \
        -Dgallium-vdpau=disabled \
        -Dgallium-vs=disabled \
        -Dvulkan-drivers=swrast,intel,virtio-experimental \
        -Dvalgrind=disabled
fi
ninja -C build/docker install

popd

#apt install -y cmake
#
#git clone https://github.com/libsdl-org/SDL sdl2
#pushd sdl2
#cmake -S. -Bbuild
#cmake --build build --target install
#popd
#
#git clone https://github.com/libsdl-org/SDL_image sdl2-image
#pushd sdl2-image
#cmake -S. -Bbuild
#cmake --build build --target install
#popd