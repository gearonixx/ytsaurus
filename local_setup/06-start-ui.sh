#!/usr/bin/env bash
# Поднимает YTsaurus Web UI в docker-контейнере на ХОСТЕ, подключённый к
# нашему локальному кластеру.
#
# Архитектура:
#   YT-кластер (master, http_proxy, node, scheduler, controller_agent)
#       — крутится нативно на хосте, http_proxy на :8000
#   yt-ui (ghcr.io/ytsaurus/ui:stable)
#       — крутится в docker с --network host, nginx слушает порт 80 хоста
#       — внутрь идёт по localhost:8000 к нашему http_proxy

set -e

CLUSTER_NAME="${CLUSTER_NAME:-local}"
PROXY_HOST="${PROXY_HOST:-localhost}"        # как UI покажет адрес кластера юзеру в браузере
PROXY_PORT="${PROXY_PORT:-8000}"
INTERFACE_PORT="${INTERFACE_PORT:-80}"       # на каком порту хоста будет UI (с --network host берётся напрямую из контейнера)
UI_IMAGE="${UI_IMAGE:-ghcr.io/ytsaurus/ui:stable}"
UI_NAME="${UI_NAME:-yt-ui}"

# Снести предыдущий, если был
docker rm -f "$UI_NAME" >/dev/null 2>&1 || true

echo "=== Pull образа $UI_IMAGE ==="
docker pull "$UI_IMAGE"

echo
echo "=== Запускаю UI ==="
# Переменные окружения (взяты из официального run_local_cluster.sh):
#   YT_LOCAL_CLUSTER_ID — id кластера, который покажется в UI слева
#   PROXY               — публичный адрес кластера (используется UI для генерации
#                         ссылок, по которым ходит браузер пользователя)
#   PROXY_INTERNAL      — адрес, по которому САМ UI-сервер ходит на cluster API
#                         (с --network host → localhost:8000)
#   APP_ENV=local       — режим интерфейса без авторизации/TVM
#   APP_INSTALLATION=custom — кастомный installation (без яндексовых dashboard'ов)
docker run -d \
    --name "$UI_NAME" \
    --network host \
    -e YT_LOCAL_CLUSTER_ID="$CLUSTER_NAME" \
    -e PROXY="${PROXY_HOST}:${PROXY_PORT}" \
    -e PROXY_INTERNAL="localhost:${PROXY_PORT}" \
    -e APP_ENV=local \
    -e APP_INSTALLATION=custom \
    -e ALLOW_PASSWORD_AUTH=1 \
    "$UI_IMAGE"

echo
echo "=== Подождём пару секунд и покажем логи ==="
sleep 3
docker logs --tail 30 "$UI_NAME" || true

echo
echo "=== Статус ==="
docker ps --filter "name=$UI_NAME"

echo
echo "Открой в браузере: http://localhost/"
echo
echo "Логин в UI: пользователь 'root', пароль пустой (так настроен local cluster без auth)."
echo
echo "Остановить UI:"
echo "    docker rm -f $UI_NAME"
