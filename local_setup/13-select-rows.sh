#!/usr/bin/env bash
# Простой smoke-тест `yt select-rows`: выполняет агрегатный запрос
# по динамической таблице и печатает результат в pretty-yson.
#
# Зависимости:
#   * venv с yt CLI (source ~/yt-venv/bin/activate)
#   * поднятый кластер (./up или ./05-start-cluster.sh)
#   * существующая динамическая таблица (см. 11-test-dyn-table-rpc.sh,
#     которая создаёт //home/dyn_table_rpc_N)
#
# Использование:
#   ./13-select-rows.sh                       # возьмёт последний dyn_table_rpc_*
#   ./13-select-rows.sh //path/to/table       # явный путь

set -e

export YT_PROXY="${YT_PROXY:-localhost:8000}"

command -v yt >/dev/null || { echo "yt CLI не в PATH. Активируй venv: source ~/yt-venv/bin/activate"; exit 1; }

if [ -n "$1" ]; then
    table="$1"
else
    last=$(yt list //home 2>/dev/null | grep -E '^dyn_table_rpc_[0-9]+$' | sed 's/^dyn_table_rpc_//' | sort -n | tail -1)
    if [ -z "$last" ]; then
        echo "В //home нет dyn_table_rpc_*. Сначала ./11-test-dyn-table-rpc.sh или передай путь аргументом."
        exit 1
    fi
    table="//home/dyn_table_rpc_${last}"
fi

query="sum(1) as cnt from [${table}] group by 1"

echo "=== select-rows на ${table} ==="
echo "query: ${query}"
echo
yt select-rows "$query" --format '<format=pretty>yson'
