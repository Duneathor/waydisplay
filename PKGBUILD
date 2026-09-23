# Maintainer: Local WayDisplay build
# Local-only recipe: run makepkg -si at the checkout root.
# Build the current working tree directly, including uncommitted edits.

pkgname=waydisplay
pkgver=0.1.0
pkgrel=44
pkgdesc='Low-latency remote Wayland display (SDL3 client and wlroots compositor)'
arch=('x86_64')
license=('AGPL-3.0-only')
depends=(
  'glibc' 'gcc-libs' 'liburing' 'zstd'
  'sdl3' 'wlroots0.20' 'wayland' 'libxkbcommon' 'pixman' 'libdrm'
  'ffmpeg' 'libva' 'libpipewire' 'pipewire' 'opus'
  'xorg-xwayland' 'vulkan-icd-loader' 'konsole'
)
makedepends=(
  'gcc' 'cmake' 'make' 'pkgconf'
  'wayland-protocols' 'vulkan-headers'
)
optdepends=(
  'vulkan-driver: hardware-specific Vulkan ICD for SDL3/Vulkan rendering'
  'libva-utils: vainfo for checking hardware video encoding'
  'intel-media-driver: VA-API on supported Intel GPUs'
  'mesa: VA-API on supported AMD GPUs'
  'x264: software H.264 encoding (if supported by installed FFmpeg)'
  'x265: software H.265 encoding (if supported by installed FFmpeg)'
)
# No source archive: this local recipe intentionally builds the current checkout.
# Do not publish as an AUR recipe without replacing this with pinned sources.
source=()

# makepkg parses this file before its cleanbuild / cleanup operations. Never
# let those options remove the checkout's real src/ directory. For -C/-c,
# configure an out-of-tree BUILDDIR in makepkg.conf (see BUILDING.md).
if [[ ${srcdir:-} == "${startdir:-}/src" && -f "${startdir:-}/src/common/wd_time.c" ]] &&
   (( ${CLEANBUILD:-0} || ${CLEANUP:-0} )); then
  printf 'Refusing makepkg -C/-c: its srcdir is WayDisplay source code.\n' >&2
  printf 'Set BUILDDIR outside the checkout before running a clean build.\n' >&2
  return 1
fi

prepare() {
  [[ -f "$startdir/CMakeLists.txt" && -f "$startdir/LICENSE" ]] || {
    printf "Run makepkg from the WayDisplay repository root.\n" >&2
    return 1
  }
  # Stop on missing optional backends rather than silently packaging only a client.
  local required=(
    liburing libzstd sdl3 wlroots-0.20
    wayland-server wayland-protocols xkbcommon pixman-1 libdrm
    libavcodec libavutil libswscale libpipewire-0.3 opus
  )
  pkg-config --print-errors --exists "${required[@]}"
  pkg-config --atleast-version=0.20.0 wlroots-0.20 || {
    printf "wlroots >= 0.20.0 is required for the server.\n" >&2
    return 1
  }
  command -v wayland-scanner >/dev/null
}

# Release defines NDEBUG in this project; its assert-based tests would be inert.
# Build a separate Debug test tree so check() runs meaningful assertions.
_configure_waydisplay() {
  local tree=$1 profile=$2 tests=$3
  # INFO is the lightweight production compile-time ceiling. Use -v at
  # runtime to show compiled-in diagnostics (including STATS/DEBUG builds).
  # The Debug CTest tree always retains DEBUG regardless of this override.
  local release_log_level=${WAYDISPLAY_PACKAGE_LOG_LEVEL:-INFO}
  case $release_log_level in
    OFF|ERROR|WARN|INFO|STATS|DEBUG) ;;
    *) printf 'Unsupported WAYDISPLAY_PACKAGE_LOG_LEVEL: %s\n' "$release_log_level" >&2; return 1 ;;
  esac
  local -a _debug_flags=()
  if [[ $profile == Debug ]]; then
    # Arch can define fortify via -Wp,-D_FORTIFY_SOURCE=3. GCC processes
    # -Wp flags after ordinary -U, so explicitly undo that definition in
    # the preprocessor as well. Debug keeps -O0/assertions; Release retains
    # the host's fortification and optimization flags.
    _debug_flags=(
      '-DCMAKE_C_FLAGS_DEBUG=-g -U_FORTIFY_SOURCE -Wp,-U_FORTIFY_SOURCE'
      '-DCMAKE_CXX_FLAGS_DEBUG=-g -U_FORTIFY_SOURCE -Wp,-U_FORTIFY_SOURCE'
    )
  fi
  local log_level=DEBUG
  if [[ $profile == Release ]]; then
    log_level=$release_log_level
  fi
  cmake -S "$startdir" -B "$srcdir/$tree" \
    -DWAYDISPLAY_LOG_LEVEL="$log_level" \
    -G 'Unix Makefiles' \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_BUILD_TYPE="$profile" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DWAYDISPLAY_BUILD_CLIENT_SDL=ON \
    -DWAYDISPLAY_BUILD_WLROOTS_SERVER=ON \
    -DWAYDISPLAY_REQUIRE_RUNTIME_TARGETS=ON \
    -DWAYDISPLAY_BUILD_TESTS="$tests" \
    -DWAYDISPLAY_RUN_TESTS_ON_BUILD=OFF \
    -DWAYDISPLAY_REQUIRE_CODEC_TESTS="$tests" \
    -DWAYDISPLAY_ENABLE_XWAYLAND=ON \
    -DWAYDISPLAY_ENABLE_AUDIO=ON \
    -DWAYDISPLAY_ENABLE_H264_SERVER_ENCODER=ON \
    -DWAYDISPLAY_ENABLE_H264_CLIENT_DECODER=ON \
    -DWAYDISPLAY_ENABLE_H265_SERVER_ENCODER=ON \
    -DWAYDISPLAY_ENABLE_H265_CLIENT_DECODER=ON \
    -DWAYDISPLAY_ENABLE_AV1_SERVER_ENCODER=ON \
    -DWAYDISPLAY_ENABLE_AV1_CLIENT_DECODER=ON \
    -DWAYDISPLAY_ENABLE_VAAPI_CLIENT_DECODER=ON \
    "${_debug_flags[@]}"
}

build() {
  _configure_waydisplay build-release Release OFF
  cmake --build "$srcdir/build-release" --parallel "$(nproc)"

  _configure_waydisplay build-tests Debug ON
  cmake --build "$srcdir/build-tests" --parallel "$(nproc)"
}

check() {
  ctest --test-dir "$srcdir/build-tests" --output-on-failure
}

package() {
  DESTDIR="$pkgdir" cmake --install "$srcdir/build-release"
  install -Dm644 "$startdir/LICENSE" \
    "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
  test -x "$pkgdir/usr/bin/waydisplay-client"
  test -x "$pkgdir/usr/bin/waydisplay-server"
}
