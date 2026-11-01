#!/usr/bin/env bash
# compile.sh — md2pdf multi-architecture (amd64 / arm64 / riscv64)
set -e

export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-}"
export ZERO_AR_DATE=1

SRC_FILES="src/main.c src/pdf_core.c src/parser.c src/image.c src/http.c"

if [ -z "$1" ]; then
    echo "Usage: $0 [amd64|arm64|riscv64|all]"
    exit 0
fi

ARCH="$1"

build_one() {
    local TARGET="$1"
    local PLATFORM BIN CFLAGS_ARCH

    case "$TARGET" in
        amd64) PLATFORM="linux/amd64"; BIN="micro-md2pdf-amd64"; CFLAGS_ARCH="-march=core2 -mtune=core2" ;;
        arm64) PLATFORM="linux/arm64"; BIN="micro-md2pdf-arm64"; CFLAGS_ARCH="-march=armv8-a" ;;
        riscv64) PLATFORM="linux/riscv64"; BIN="micro-md2pdf-riscv64"; CFLAGS_ARCH="-march=rv64gc" ;;
        *) echo "❌ Unknown architecture: $TARGET"; return 1 ;;
    esac

    echo "========================================================================="
    echo "  micro-md2pdf — building for $TARGET ($PLATFORM)"
    echo "========================================================================="

    CFLAGS_BASE="-Os $CFLAGS_ARCH -fomit-frame-pointer -fno-ident -ffunction-sections -fdata-sections -DNDEBUG -std=c99 -D_DEFAULT_SOURCE"

    HAS_DOCKER=false
    DOCKER_RUNNING=false
    if command -v docker &> /dev/null; then
        HAS_DOCKER=true
        if docker info &> /dev/null; then DOCKER_RUNNING=true; fi
    fi

    if [ "$HAS_DOCKER" = false ]; then
        echo "❌ [ERROR] Docker no está instalado en este sistema."
        echo "Este script requiere Docker para realizar la compilación cruzada multi-arquitectura."
        return 1
    elif [ "$DOCKER_RUNNING" = false ]; then
        echo "❌ [ERROR] El servicio de Docker está instalado pero NO se está ejecutando."
        echo "Por favor, inicia el demonio de Docker (e.g., 'sudo systemctl start docker' o abre Docker Desktop) e inténtalo de nuevo."
        return 1
    fi

    docker run --rm \
    --platform "$PLATFORM" \
    -v "$PWD:/work" -w /work \
    alpine:latest sh -c "
    set -e
    echo \"[*] Installing base dependencies in Alpine ($TARGET)...\"
    apk add --no-cache build-base cmake git meson ninja pkgconfig nasm \
        zlib-dev zlib-static linux-headers perl py3-jsonschema py3-jinja2

    echo \"[*] Compiling static MbedTLS (v3.6.0 LTS)...\"
    git clone --progress --depth 1 --branch mbedtls-3.6 --recurse-submodules https://github.com/Mbed-TLS/mbedtls.git /tmp/mbedtls
    cd /tmp/mbedtls && mkdir -p build && cd build
    cmake .. \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_PROGRAMS=OFF \
        -DENABLE_TESTING=OFF \
        -DCMAKE_C_FLAGS=\"-Wno-error=unterminated-string-initialization\" \
        -DCMAKE_INSTALL_PREFIX=/usr
    make -j\$(nproc) && make install

    echo \"[*] Compiling static libwebp...\"
    git clone --progress --depth 1 https://chromium.googlesource.com/webm/libwebp /tmp/libwebp
    cd /tmp/libwebp && mkdir -p build && cd build
    cmake .. \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DBUILD_SHARED_LIBS=OFF \
        -DWEBP_ENABLE_SIMD=OFF \
        -DWEBP_BUILD_LIBWEBPMUX=OFF \
        -DWEBP_BUILD_WEBPINFO=OFF \
        -DWEBP_BUILD_ANIM_UTILS=OFF \
        -DWEBP_BUILD_CWEBP=OFF \
        -DWEBP_BUILD_DWEBP=OFF \
        -DWEBP_BUILD_GIF2WEBP=OFF \
        -DWEBP_BUILD_IMG2WEBP=OFF \
        -DWEBP_BUILD_VWEBP=OFF \
        -DWEBP_BUILD_WEBPMUX=OFF \
        -DWEBP_BUILD_EXTRAS=OFF \
        -DWEBP_BUILD_WEBP_JS=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr
    make -j\$(nproc) && make install

    echo '[*] Compiling dav1d...'
    git clone --progress --depth 1 https://code.videolan.org/videolan/dav1d.git /tmp/dav1d
    cd /tmp/dav1d && meson setup build \
        --default-library=static \
        --buildtype=minsize \
        --prefix=/usr \
        -Dbitdepths=8 \
        -Denable_asm=false \
        -Denable_tools=false \
        -Denable_examples=false \
        -Denable_tests=false
    ninja -C build && ninja -C build install

    echo \"[*] Preparing static libavif...\"
    git clone --progress --depth 1 --branch v1.0.4 https://github.com/AOMediaCodec/libavif.git /tmp/libavif
    cd /tmp/libavif && mkdir -p build && cd build
    cmake .. \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DBUILD_SHARED_LIBS=OFF \
        -DAVIF_CODEC_AOM=OFF \
        -DAVIF_CODEC_DAV1D=ON \
        -DAVIF_BUILD_APPS=OFF \
        -DAVIF_BUILD_EXAMPLES=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr
    make -j\$(nproc) && make install

    cd /work
    echo \"[*] Compiling 100% static monolithic binary for $TARGET...\"
    gcc $CFLAGS_BASE $SRC_FILES -o $BIN -static \
        -lavif -lsharpyuv -ldav1d -lwebpdecoder -lmbedtls -lmbedx509 -lmbedcrypto -lz -lpthread -lm \
        -Wl,--gc-sections -s" 2>&1

    if [ -f "$BIN" ]; then
        SIZE_BYTES=$(wc -c < "$BIN" | tr -d ' ')
        SIZE_MB=$(awk "BEGIN {printf \"%.2f\", $SIZE_BYTES/1048576}")
        echo "========================================================================="
        echo "✅ [SUCCESS] Binary for $TARGET generated successfully."
        echo "📦 Location: ./$BIN"
        echo "📏 Size: ${SIZE_MB} MB ($SIZE_BYTES bytes)"
        echo "========================================================================="
        return 0
    else
        echo "❌ [ERROR] Binary build failed."
        return 1
    fi
}

case "$ARCH" in
    all)
        echo "🚀 Building for ALL architectures..."
        echo ""
        for t in amd64 arm64 riscv64; do
            build_one "$t" || exit 1
            echo ""
        done
        echo "========================================================================="
        echo "📦 Final binaries:"
        for b in micro-md2pdf-amd64 micro-md2pdf-arm64 micro-md2pdf-riscv64; do
            if [ -f "$b" ]; then
                SIZE_BYTES=$(wc -c < "$b" | tr -d ' ')
                SIZE_MB=$(awk "BEGIN {printf \"%.2f\", $SIZE_BYTES/1048576}")
                echo "  $b: ${SIZE_MB} MB ($SIZE_BYTES bytes)"
            fi
        done
        file micro-md2pdf-{amd64,arm64,riscv64} 2>/dev/null || true
        echo "========================================================================="
        ;;
    amd64|arm64|riscv64)
        build_one "$ARCH"
        ;;
    *)
        echo "❌ Invalid argument: $ARCH"
        echo ""
        echo "Usage: $0 [amd64|arm64|riscv64|all]"
        echo ""
        echo "  amd64   → x86_64 (Intel/AMD)"
        echo "  arm64   → aarch64 (ARM 64-bit)"
        echo "  riscv64 → RISC-V 64-bit"
        echo "  all     → build all three architectures at once"
        exit 1
        ;;
esac

