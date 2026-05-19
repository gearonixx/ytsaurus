#!/usr/bin/env bash
# Smoke-тест для ДИНАМИЧЕСКОЙ таблицы через RPC-прокси.
# Аналог 10-test-table-rpc.sh, но вместо write-table/read-table —
# полный жизненный цикл dyn-таблицы: create(dynamic=%true) → mount → insert-rows → select-rows.
#
# Отличия от статической таблицы (см. 09/10):
#   * В @schema каждая sort-колонка должна иметь sort_order=ascending,
#     иначе таблица будет ordered (без ключа) и lookup/select по ключу не работает.
#   * Сама cypress-нода создаётся в состоянии "unmounted" — реально на tablet'е
#     её ещё нет. Любые insert/select упадут с "table is not mounted".
#     Нужно `yt mount-table` и дождаться @tablet_state == "mounted".
#   * Для mount должен быть хотя бы один healthy tablet cell в //sys/tablet_cells.
#     yt_local поднимает один по умолчанию (см. yt_env.py:tablet_cell_bundle).
#
# RPC-часть — ровно как в 10-test-table-rpc.sh:
#   YT_CONFIG_PATCHES={backend=rpc;} переключает get_backend_type на rpc (config.py:84),
#   make_request уходит в native_driver → yt_driver_rpc_bindings (C++ Driver).

set -e

export YT_PROXY="${YT_PROXY:-localhost:8000}"

# --- Форсим venv'овский yt (см. 10-test-table-rpc.sh про сломанный pipx) ----
VENV="${VENV:-$HOME/yt-venv}"
[ -x "$VENV/bin/yt" ] || { echo "Нет $VENV/bin/yt. Запусти 03-install-host-python.sh"; exit 1; }
export PATH="$VENV/bin:$PATH"

# --- Проверка RPC-биндингов в интерпретаторе yt ----------------------------
YT_BIN="$(command -v yt)"
YT_PY="$(head -1 "$YT_BIN" | sed -E 's|^#!\s*||')"
[ -x "$YT_PY" ] || YT_PY="$(python -c 'import sys; print(sys.executable)')"
echo "yt CLI:        $YT_BIN"
echo "yt interpreter: $YT_PY"

if ! "$YT_PY" -c "from yt_driver_rpc_bindings import Driver" 2>/dev/null; then
    echo "=== Ставим ytsaurus-rpc-driver в интерпретатор yt ($YT_PY) ==="
    case "$YT_PY" in
        */pipx/venvs/ytsaurus-client/*)
            pipx inject ytsaurus-client ytsaurus-rpc-driver
            ;;
        *)
            "$YT_PY" -m pip install --quiet ytsaurus-rpc-driver
            ;;
    esac
fi

# --- RPC-прокси в кластере -------------------------------------------------
rpc_proxies=$(yt list //sys/rpc_proxies)
if [ -z "$rpc_proxies" ]; then
    echo "В //sys/rpc_proxies пусто — перезапусти 05-start-cluster.sh с --rpc-proxy-count 1."
    exit 1
fi
echo "=== RPC-прокси ==="
echo "$rpc_proxies"

# --- Tablet cells -----------------------------------------------------------
# Без healthy cell mount-table повиснет/упадёт. У yt_local дефолтно один cell
# создаётся в bootstrap'е (yt_env_setup), но если кластер кастомный — проверим.
cells=$(yt list //sys/tablet_cells)
if [ -z "$cells" ]; then
    echo "Нет tablet_cells. Динамические таблицы монтировать некуда."
    echo "Создай cell вручную:"
    echo "    cell_id=\$(yt create tablet_cell --attributes '{size=1}')"
    echo "    yt set //sys/tablet_cells/\$cell_id/@... (см. доки)"
    exit 1
fi
echo "=== Tablet cells ==="
echo "$cells"

# --- Переключаем backend в RPC --------------------------------------------
export YT_CONFIG_PATCHES='{backend=rpc;}'

# --- Следующий свободный номер ---------------------------------------------
existing=$(yt list //home 2>/dev/null | grep -E '^dyn_table_rpc_[0-9]+$' || true)
if [ -z "$existing" ]; then
    n=1
else
    max=$(echo "$existing" | sed 's/^dyn_table_rpc_//' | sort -n | tail -1)
    n=$((max + 1))
fi
table="//home/dyn_table_rpc_${n}"

echo
echo "=== Создаю $table (dynamic=%true, через RPC) ==="
# sort_order=ascending у id => sorted dynamic table (поддерживает lookup/select по ключу).
# Без sort_order таблица была бы ordered — insert работает, lookup-by-key — нет.
yt create table "$table" --attributes '{
    dynamic=%true;
    schema=[
        {name=id; type=int64; sort_order=ascending};
        {name=text; type=string};
    ]
}'

echo
echo "=== Mount $table ==="
yt mount-table "$table"

# Mount асинхронный: cypress-узел сразу возвращает control, а реальный tablet
# поднимается на cell'е чуть позже. Поллим @tablet_state.
echo -n "Жду state=mounted"
for i in $(seq 1 30); do
    # У нас в drivers/native_driver/heavy_commands расставлен debug-логгинг,
    # который пишется в stdout (а не в stderr) и попадает в подстановку.
    # Фильтруем по префиксам нашего лога, чтобы остался только реальный YSON.
    raw=$(yt get "${table}/@tablet_state" 2>&1)
    # yt get печатает значение без \n в конце, и наш debug-лог приклеивается
    # к той же строке: '"mounted"[gearonixx] ...'. Поэтому режем от первого
    # вхождения '[gearonixx]'/'[make_request]', а не по началу строки.
    state=$(echo "$raw" | sed -E 's/\[(gearonixx|make_request)\].*//' | tr -d '"' | tr -d '[:space:]')
    if [ "$state" = "mounted" ]; then
        echo " — ok ($state)"
        break
    fi
    echo -n "."
    sleep 1
done
if [ "$state" != "mounted" ]; then
    echo
    echo "Таблица так и не смонтировалась (state=$state). Проверь //sys/tablet_cells/*/@health."
    exit 1
fi

echo
echo "=== Insert в $table (через RPC) ==="
# insert-rows ждёт JSONL на stdin — по одной row на строку, без массива-обёртки.
# В отличие от write-table это транзакционная вставка в tablet, не append-в-chunk.
printf '%s\n' \
    '{"id": 0, "text": "Hello"}' \
    '{"id": 1, "text": "World!"}' \
    '{"id": 2, "text": "dyn via rpc"}' \
    | yt insert-rows "$table" --format json

echo
echo "=== Select из $table (через RPC) ==="
yt select-rows "* from [${table}]" --format json
