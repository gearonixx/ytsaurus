#!/usr/bin/env bash
# Smoke-тест: создаёт //home/input_table_N, пишет туда пару строк и читает обратно.
# N — инкрементальный номер: на каждый запуск +1, чтобы не пересоздавать одну и ту же таблицу.
# Перед запуском должен быть активирован venv (source ~/yt-venv/bin/activate)
# и поднят кластер (./05-start-cluster.sh).

set -e

export YT_PROXY="${YT_PROXY:-localhost:8000}"

command -v yt >/dev/null || { echo "yt CLI не в PATH. Активируй venv: source ~/yt-venv/bin/activate"; exit 1; }

# Найти следующий свободный номер. Cписаем существующие input_table_* в //home
# и возьмём max+1. Если ничего нет — стартуем с 1.
existing=$(yt list //home 2>/dev/null | grep -E '^input_table_[0-9]+$' || true)
if [ -z "$existing" ]; then
    n=1
else
    max=$(echo "$existing" | sed 's/^input_table_//' | sort -n | tail -1)
    n=$((max + 1))
fi

table="//home/input_table_${n}"
echo "=== Создаю $table ==="
yt create table "$table" --attributes '{schema = [{name = id; type = int64}; {name = text; type = string}]}'

echo
echo "=== Пишу в $table ==="
echo '{"id": 0, "text": "Hello"} {"id": 1, "text": "World!"}' \
    | yt write-table "$table" --format json

echo
echo "=== Читаю $table ==="
yt read-table "$table" --format json
