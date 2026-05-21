#!/usr/bin/env bash
# Запуск локального YT-кластера на ХОСТЕ.
# Перед запуском: venv должен быть активирован (source ~/yt-venv/bin/activate)
# и должен существовать /tmp/ytserver-all.

set -e

YTSERVER_ALL="${YTSERVER_ALL:-/tmp/ytserver-all}"
PROXY_PORT="${PROXY_PORT:-8000}"
FQDN="${FQDN:-localhost}"
WORKDIR="${WORKDIR:-/tmp/yt_local}"
RPC_PROXY_COUNT="${RPC_PROXY_COUNT:-1}"

# Проверки окружения
command -v yt_local >/dev/null || { echo "yt_local не в PATH. Активируй venv: source ~/yt-venv/bin/activate"; exit 1; }
[ -x "$YTSERVER_ALL" ]         || { echo "Нет $YTSERVER_ALL. Запусти 04-copy-binary.sh"; exit 1; }

# yt_local — это всего лишь shebang на /usr/bin/env python3 в исходниках, он
# не падает, если python не может импортировать yt.local. Видимый симптом —
# `ModuleNotFoundError: No module named 'yt.local'` при попытке стартануть.
# Чаще всего это означает: editable-инсталл `ytsaurus-client-trunk-dev/build/`
# не подключён (см. историю в 03-install-host-python.sh). Падаем заранее
# с понятным сообщением.
if ! python -c "import yt.local" 2>/dev/null; then
    echo "import yt.local упал — editable-инсталл сломан или не сделан."
    echo "Перезапусти 03-install-host-python.sh (он перезальёт build/ и проверит импорт)."
    exit 1
fi
[ "$(cat /proc/sys/vm/overcommit_memory)" = "1" ] || echo "WARN: vm.overcommit_memory != 1 — запусти 02-prepare-host.sh"

# Поднять soft-лимит на открытые файлы.
# По умолчанию systemd / docker / login дают 1024 (это soft, hard обычно 524288).
# YT-сервер регулярно открывает 100+ файлов и сокетов. Без этого через минуты
# работы упёрся бы в EMFILE.
ulimit -n 524288 || { echo "Не получилось поднять nofile"; exit 1; }
echo "ulimit -n = $(ulimit -n)"

# Прибрать старые инстансы (там могут остаться pid-файлы от убитого yt_local)
mkdir -p "$WORKDIR"
cd "$WORKDIR"
rm -rf ./*  # сносим только содержимое /tmp/yt_local/*, не сам каталог

# Запуск. Флаги:
#   --enable-debug-logging
#       включает debug-уровень для всех компонентов сразу. Без него debug-логи
#       пишет только controller-agent (это поведение зашито в configs_provider.py),
#       и YT_LOG_DEBUG из http_proxy/master/node/scheduler никуда не попадают.
#       (Этот флаг помечен deprecated в пользу --log-level, но пока работает.)
#   --enable-structured-logging
#       включает дополнительные writer'ы в JSON-формате. В logs/ появятся
#       *.json.log файлы рядом с обычными *.log/*.debug.log. Каждое сообщение
#       это отдельная JSON-строка с полями timestamp, level, category, message,
#       trace_id, request_id и attributes (включая всё, что пришло через
#       TErrorAttribute / YT_LOG_*_WITH_TAGS). Грепать структурой через jq:
#           jq 'select(.level=="ERROR") | {ts:.timestamp,msg:.message,attrs:.attributes}' \
#               /tmp/yt_local/*/logs/http-proxy-0.json.log
#   --proxy-port 8000
#       HTTP-порт, через который кластер слушает API. На него же будет ходить UI.
#   --fqdn localhost
#       что писать в адреса серверов внутри Cypress. Можно `127.0.0.1` или реальный hostname.
#   --ytserver-all-path
#       наш свежий бинарь. yt_local запустит его несколько раз с разными аргументами
#       (как master, как http_proxy, как scheduler и т.д. — это multi-call бинарь).
#   --sync
#       блокирующий режим. Команда вернёт управление только когда кластер реально
#       поднялся и прошёл healthcheck (или упал на старте). Ctrl+C для остановки.
#       Без --sync команда форкается и сразу выходит, кластер живёт в фоне.
#   --rpc-proxy-count N
#       сколько RPC-прокси поднять. По умолчанию yt_local запускает 0 — тогда
#       //sys/rpc_proxies пуст и любой клиент с backend=rpc упадёт на discovery
#       (см. 10-test-table-rpc.sh). Ставим 1, чтобы и HTTP, и RPC backend работали.
echo
echo "=== yt_local start (Ctrl+C для остановки) ==="
# EXTRA_YT_LOCAL_ARGS — точка расширения для опциональных компонентов
# (например, --component '{name=query_tracker}'). По умолчанию пусто →
# минимальный кластер. См. ./up, который выставляет это для QT.
# shellcheck disable=SC2086
exec yt_local start \
    --enable-debug-logging \
    --enable-structured-logging \
    --proxy-port "$PROXY_PORT" \
    --fqdn "$FQDN" \
    --rpc-proxy-count "$RPC_PROXY_COUNT" \
    --ytserver-all-path "$YTSERVER_ALL" \
    $EXTRA_YT_LOCAL_ARGS \
    --sync

# После старта смотри лог в этом же терминале (он будет показывать INFO-сообщения),
# а dump'ы компонентов — в другом терминале через 07-tail-logs.sh.
