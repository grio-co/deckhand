# Build and test environment for the Linux port (Ubuntu 24.04). Used by scripts/linux/docker-build.sh.
# Ubuntu 24.04 has no SDL3 package, so it is built from a pinned release into /usr/local.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential clang cmake ninja-build pkg-config git ca-certificates curl rsync \
    libsqlcipher-dev libssl-dev zlib1g-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libasound2-dev \
    libpango1.0-dev libcairo2-dev libfontconfig-dev \
    libgl-dev libegl-dev libgles-dev libgbm-dev libdrm-dev libudev-dev libdbus-1-dev libxkbcommon-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
    libwayland-dev libwayland-bin wayland-protocols libdecor-0-dev \
    fonts-dejavu-core fonts-noto-core fonts-noto-cjk \
    xvfb xauth x11-utils xdotool libgl1-mesa-dri mesa-utils imagemagick ffmpeg \
 && rm -rf /var/lib/apt/lists/*
ARG SDL_TAG=release-3.4.20
RUN git clone --depth 1 --branch ${SDL_TAG} https://github.com/libsdl-org/SDL /tmp/sdl \
 && cmake -S /tmp/sdl -B /tmp/sdl/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
      -DSDL_CAMERA=OFF -DSDL_VULKAN=OFF \
 && cmake --build /tmp/sdl/b && cmake --install /tmp/sdl/b && ldconfig && rm -rf /tmp/sdl
WORKDIR /work
