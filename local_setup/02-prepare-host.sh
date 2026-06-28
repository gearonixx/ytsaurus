#!/usr/bin/env bash
# Подготовка ХОСТА (не контейнера).
# Запускать ОДИН РАЗ. Меняет ядерные sysctl'ы и user-лимиты.
# Нужен root (sudo). Скрипт идемпотентен — можно запускать повторно.

set -e

echo "=== 1. Проверяем, что есть sudo и /etc/sysctl.d ==="
sudo -v || { echo "Нужен sudo"; exit 1; }

echo
echo "=== 2. vm.overcommit_memory=1 ==="
# Без overcommit=1 ядро в эвристическом режиме (0) отказывает большим резервациям
# виртуальной памяти, которые делает tcmalloc/ytalloc на старте YT-процессов.
# Симптом без этой настройки: master/http_proxy падают с
#   *** Out-of-memory during object allocation
# ещё на этапе старта (до того, как кластер успевает поднять http_proxy).
sudo sysctl -w vm.overcommit_memory=1

echo
echo "=== 3. vm.max_map_count=1048576 ==="
# tcmalloc внутри YT-бинарей делает много мелких mmap-регионов под arena'ы.
# Дефолтный лимит 65536 на больших процессах может закончиться → ENOMEM от mmap → null от malloc → AbortOnOom.
sudo sysctl -w vm.max_map_count=1048576

echo
echo "=== 4. Запишем настройки на постоянку ==="
# /etc/sysctl.d/99-yt.conf переживает ребут хоста.
sudo tee /etc/sysctl.d/99-yt.conf >/dev/null <<EOF
# YTsaurus local cluster sysctls — added by local_setup/02-prepare-host.sh
vm.overcommit_memory=1
vm.max_map_count=1048576
EOF

echo
echo "=== 5. Проверка ==="
echo "vm.overcommit_memory = $(cat /proc/sys/vm/overcommit_memory)  (ждали 1)"
echo "vm.max_map_count     = $(cat /proc/sys/vm/max_map_count)      (ждали 1048576)"

echo
echo "OK"
echo
echo "Не забывай в каждой новой сессии шелла, где будешь запускать yt_local:"
echo "    ulimit -n 524288"
echo "Это поднимает soft-лимит на открытые файлы (по умолчанию 1024)."
echo "YT-серверы открывают много сокетов/changelog'ов/снапшотов, без этого через"
echo "некоторое время процесс упирается в EMFILE и аллокации в обработчиках ошибок"
echo "идут в путь, который зовёт AbortOnOom."
