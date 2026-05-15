#!/usr/bin/env bash
# Копирует свежесобранный ytserver-all из контейнера на хост.
# Запускать на ХОСТЕ. Делать каждый раз после пересборки в контейнере.
#
# Почему `docker cp`, а не запуск напрямую из bind-mounted /workspace:
#   /workspace в контейнере — это bind-mount твоего clone'а репо на хосте,
#   ИСХОДНИКИ доступны напрямую. Но build/ — это уже внутренняя папка контейнера
#   (там cmake кэш, .o-файлы, итоговые бинари). Чтобы запустить бинарь на хосте,
#   надо его вынуть из контейнера.
#
# Если хочется этого избежать — можно перенести build-каталог в bind-mount:
#   * остановить контейнер
#   * docker run ... -v /home/x/try3/build:/workspace/build ...
#   * пересобрать. Тогда docker cp не нужен.

set -e

CONTAINER="${CONTAINER:-ytsaurus-dev}"
SRC="/workspace/build/yt/yt/server/all/ytserver-all"
DST="${DST:-/tmp/ytserver-all}"

echo "=== Копирую $CONTAINER:$SRC → $DST ==="
docker cp "$CONTAINER:$SRC" "$DST"
chmod +x "$DST"

echo
echo "=== Проверка ==="
ls -la "$DST"
file "$DST" | tr ',' '\n' | head -3

echo
echo "=== Версия ==="
# Если ругнётся на `GLIBC_2.XX not found` — значит host glibc старше, чем в
# контейнере. Обычно у Arch glibc новее, и наоборот: бинарь, собранный на
# Ubuntu 22.04 (glibc 2.35), на Arch (glibc 2.39+) запустится; обратное — нет.
# При несовместимости варианты:
#   * пересобрать с `-DCMAKE_TOOLCHAIN_FILE=...` указав более старый glibc
#   * вытащить из контейнера ld-linux + libc и запускать через своё интерпретер
"$DST" --version
