#!/usr/bin/env bash
# Поднимает YTsaurus Web UI в docker-контейнере на ХОСТЕ, подключённый к
# нашему локальному кластеру.
#
# Архитектура:
#   YT-кластер (master, http_proxy, node, scheduler, controller_agent)
#       — крутится нативно на хосте, http_proxy на :8000
#   yt-ui (ghcr.io/ytsaurus/ui:stable)
#       — крутится в docker, внутри nginx + node, слушает порт 80 контейнера
#       — port-map: 8001 хоста → 80 контейнера
#       — внутрь идёт по host.docker.internal:8000 к нашему http_proxy
#
# host.docker.internal — DNS-имя для IP хоста с точки зрения контейнера.
# На macOS/Windows работает из коробки, на Linux требует флаг
#   --add-host=host.docker.internal:host-gateway

set -e

CLUSTER_NAME="${CLUSTER_NAME:-local}"
PROXY_HOST="${PROXY_HOST:-localhost}"        # как UI покажет адрес кластера юзеру в браузере
PROXY_PORT="${PROXY_PORT:-8000}"
INTERFACE_PORT="${INTERFACE_PORT:-8001}"     # на каком порту хоста будет UI
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
#                         (внутри контейнера → host.docker.internal:8000)
#   APP_ENV=local       — режим интерфейса без авторизации/TVM
#   APP_INSTALLATION=custom — кастомный installation (без яндексовых dashboard'ов)
docker run -d \
    --name "$UI_NAME" \
    --add-host=host.docker.internal:host-gateway \
    -p "${INTERFACE_PORT}:80" \
    -e YT_LOCAL_CLUSTER_ID="$CLUSTER_NAME" \
    -e PROXY="${PROXY_HOST}:${PROXY_PORT}" \
    -e PROXY_INTERNAL="host.docker.internal:${PROXY_PORT}" \
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
echo "Открой в браузере: http://localhost:${INTERFACE_PORT}"
echo
echo "Логин в UI: пользователь 'root', пароль пустой (так настроен local cluster без auth)."
echo
echo "Остановить UI:"
echo "    docker rm -f $UI_NAME"
