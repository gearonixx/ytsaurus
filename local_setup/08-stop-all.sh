#!/usr/bin/env bash
# Останавливает кластер и UI. Запускать на ХОСТЕ.

set -e

echo "=== 1. Останавливаю UI (docker) ==="
docker rm -f yt-ui 2>/dev/null || echo "  (нет контейнера yt-ui)"

echo
echo "=== 2. Останавливаю YT-кластер ==="
# Если yt_local запущен с --sync — он держит терминал и стопится Ctrl+C.
# Здесь стопим уже отвязанные инстансы (если запускали без --sync).
for id in $(yt_local list 2>/dev/null | awk '$2=="status:" && $3=="running"{print $1}'); do
    echo "  yt_local stop $id"
    yt_local stop "$id" || true
done

# Orphaned ytserver/yt_local from прерванных запусков `up` (Ctrl+C, краш
# инициализации QT, и т.п.) `yt_local list` не видит — он смотрит только в
# свой реестр. Без этого следующий http_proxy не может забиндить :8000.
# Сначала TERM, потом KILL, чтобы дать ytserver-* шанс на graceful shutdown.
echo "  pkill orphaned ytserver-* / yt_local"
pkill -TERM -f 'ytserver-(master|node|scheduler|controller-agent|http-proxy|proxy|query-tracker)' 2>/dev/null || true
pkill -TERM -f '/yt_local +start' 2>/dev/null || true
for i in 1 2 3 4 5; do
    pgrep -f 'ytserver-(master|node|scheduler|controller-agent|http-proxy|proxy|query-tracker)|/yt_local +start' >/dev/null 2>&1 || break
    sleep 1
done
pkill -KILL -f 'ytserver-(master|node|scheduler|controller-agent|http-proxy|proxy|query-tracker)' 2>/dev/null || true
pkill -KILL -f '/yt_local +start' 2>/dev/null || true
pkill -KILL -f 'yt_env_watcher' 2>/dev/null || true

echo
echo "=== 3. Опционально: чистим рабочие папки ==="
# Логи и снапшоты в /tmp/yt_local/ могут весить сотни МБ — на --enable-debug-logging
# master.debug.log быстро уходит в 500MB+ за несколько минут.
read -p "Удалить /tmp/yt_local/* и /tmp/ytserver-all? [y/N] " ans
if [ "$ans" = "y" ] || [ "$ans" = "Y" ]; then
    rm -rf /tmp/yt_local/*
    rm -f  /tmp/ytserver-all
    echo "  очищено"
fi

echo
echo "=== 4. Проверка ==="
yt_local list 2>/dev/null || true
docker ps --filter "name=yt-ui"
ls /tmp/yt_local/ 2>/dev/null | head -5 || echo "  /tmp/yt_local пуст"

echo
echo "OK"
