#!/usr/bin/env bash
# Собирает ytserver-all внутри docker-контейнера ytsaurus-dev.
# Запускать ВНУТРИ контейнера (docker exec -it ytsaurus-dev bash).
#
# При первом запуске (билд с нуля) — займёт час+.
# При инкрементальных пересборках после правки одной библиотеки — секунды-минуты.

set -e

cd /workspace/build

# # Первичный configure (выполнить один раз — если build/ ещё не сконфигурирован)
# if [ ! -f build.ninja ]; then
#     cmake -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DREQUIRED_LLVM_TOOLING_VERSION=18 \
#         -DCMAKE_TOOLCHAIN_FILE=../ytsaurus/clang.toolchain \
#         -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=../ytsaurus/cmake/conan_provider.cmake \
#         ../ytsaurus
# fi

# Собираем только то, что нужно для запуска кластера:
#   yt-server-http_proxy — отдельный таргет, удобно для итераций по http_proxy
#   ytserver-all          — multi-call бинарь, который реально запускает yt_local
ninja yt-server-http_proxy ytserver-all

echo
echo "OK: $(ls -la /workspace/build/yt/yt/server/all/ytserver-all)"
echo "    $(file /workspace/build/yt/yt/server/all/ytserver-all | cut -d',' -f1)"
echo "    Version: $(/workspace/build/yt/yt/server/all/ytserver-all --version 2>&1 || true)"
