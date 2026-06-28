#!/usr/bin/env bash
# Просмотр логов работающего yt_local-кластера.
# Запускать на ХОСТЕ в отдельном терминале.
#
# Структура:
#   /tmp/yt_local/<id>/logs/
#       http-proxy-0.log         — INFO+ от http_proxy (читаемо)
#       http-proxy-0.debug.log   — DEBUG+ от http_proxy (только если --enable-debug-logging)
#       master-0-0.log / .debug.log
#       node-0.log / .debug.log
#       scheduler-0.log / .debug.log
#       controller-agent-0.log / .debug.log
#       watcher.log              — sidecar-сторож процессов yt_local
#   /tmp/yt_local/<id>/stderrs/
#       stderr.<component>       — то, что компоненты пишут в stderr (обычно craches/stack traces)

set -e

INSTANCE_ID="$(yt_local list 2>/dev/null | awk '$2=="status:" && $3=="running"{print $1}' | head -1)"
if [ -z "$INSTANCE_ID" ]; then
    INSTANCE_ID="$(ls -1t /tmp/yt_local | head -1)"
fi

LOGS="/tmp/yt_local/$INSTANCE_ID/logs"
echo "=== Логи инстанса $INSTANCE_ID ==="
echo "Путь: $LOGS"
ls -lah "$LOGS"
echo

COMPONENT="${1:-http-proxy-0}"
LEVEL="${2:-debug}"   # debug или info
LOGFILE="$LOGS/${COMPONENT}.${LEVEL}.log"

if [ ! -f "$LOGFILE" ]; then
    echo "Нет $LOGFILE."
    echo "Если ждал debug-лог, но его нет — кластер запущен без --enable-debug-logging."
    echo
    echo "Доступные:"
    ls "$LOGS"/ | grep -v stderr
    exit 1
fi

echo "=== tail -f $LOGFILE ==="
echo "Триггер запроса для http_proxy: в другом терминале сделай"
echo "    curl -s 'http://localhost:8000/api/v4/get?path=//sys/@cluster_name'"
echo
exec tail -f "$LOGFILE"

# Полезные grep'ы (запускать руками):
#
# Только WARNING+ из всех логов:
#   grep -h '^[0-9-]* [0-9:.]*\s*[WE]\s' /tmp/yt_local/*/logs/*.log
#
# Найти сообщение про конкретный запрос:
#   grep 'Running driver request' /tmp/yt_local/*/logs/http-proxy-0.debug.log
#
# Все ошибки за последние 5 минут (если у тебя есть `ts`):
#   tail -F /tmp/yt_local/*/logs/*.log | grep -E '\s[WE]\s'
#
# Структурированные JSON-логи (если запущено с --enable-structured-logging):
#
# Все ошибки с атрибутами (включая TErrorAttribute из THROW_ERROR_EXCEPTION):
#   jq 'select(.level=="E") | {ts:.timestamp,msg:.message,attrs:.attributes}' \
#       /tmp/yt_local/*/logs/http-proxy-0.json.log
#
# Все запросы конкретного пользователя:
#   jq -c 'select(.authenticated_user=="root")' /tmp/yt_local/*/logs/http-proxy-0.json.log
#
# Найти trace_id во всех компонентах:
#   TID="fffee6b7..."
#   for f in /tmp/yt_local/*/logs/*.json.log; do
#       jq -c "select(.trace_id==\"$TID\")" "$f"
#   done
#
# Live-стрим ошибок с прокси:
#   tail -F /tmp/yt_local/*/logs/http-proxy-0.json.log | jq -c 'select(.level=="E")'
