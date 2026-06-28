#!/usr/bin/env bash
# То же, что 09-test-table.sh (create / write-table / read-table), но трафик идёт
# не через HTTP-прокси, а через RPC-прокси.
#
# Что переключает backend:
#   * yt CLI -> yt_binary.py -> yt.wrapper.table_commands.read_table/write_table
#     -> yt.wrapper.driver.make_request -> get_backend_type(client) (config.py:84).
#     Если backend=="http" -> http_driver (HTTP POST к прокси с params в X-YT-Parameters).
#     Если backend=="rpc"  -> native_driver через C++ биндинги yt_driver_rpc_bindings:
#         driver_config авто-собирается в native_driver.get_driver_instance:
#             {"connection_type": "rpc", "cluster_url": <proxy/url>}
#         RPC-эндпоинты прокси discoverятся через HTTP discovery (//sys/rpc_proxies).
#   * Backend выставляется через YT_CONFIG_PATCHES={backend=rpc;} — отдельного
#     YT_BACKEND shortcut в default_config.py нет, но YT_CONFIG_PATCHES (YSON-патч
#     поверх дефолтного конфига) парсится в default_config.py:1341.
#
# Предусловия (отличие от 09-test-table.sh):
#   1. В кластере должен быть поднят хотя бы один RPC-прокси.
#      По умолчанию yt_local стартует с rpc_proxy_count=0, RPC-прокси нет.
#      Если 05-start-cluster.sh без правок — добавь флаг к yt_local start:
#          --rpc-proxy-count 1
#      (либо --rpc-proxy — legacy-флаг, выставит count=1).
#   2. В тот python, КОТОРЫЙ исполняет `yt` CLI (а не просто в активный venv!),
#      должен быть установлен пакет yt_driver_rpc_bindings (C++ биндинг с
#      driver_rpc_lib*.so). Editable-инсталл из 03-install-host-python.sh
#      его НЕ ставит — там только pure-python обвязка.
#
#      Гранат с pipx-ом: `yt` обычно идёт из ~/.local/bin/yt с шебангом на
#      pipx-venv (~/.local/share/pipx/venvs/ytsaurus-client/bin/python).
#      `source ~/yt-venv/bin/activate` + `pip install ytsaurus-rpc-driver`
#      положит биндинг НЕ в pipx-venv, и `yt` его не увидит. Симптом:
#      `python -c "import yt_driver_rpc_bindings"` отрабатывает, а `yt create`
#      падает с "Driver class not found, install RPC driver bindings".
#      Скрипт определяет yt-овский python по shebang и ставит биндинг прямо
#      в него (через `pipx inject` если pipx, иначе через `python -m pip`).

set -e

export YT_PROXY="${YT_PROXY:-localhost:8000}"

# Форсим yt из ~/yt-venv. Иначе PATH может вытащить pipx'овский ~/.local/bin/yt
# (у него отдельный сломанный Python 3.14 без yt.packages.requests — любой `yt ...`
# валится на импорте ещё до сети). См. troubleshooting.md.
VENV="${VENV:-$HOME/yt-venv}"
[ -x "$VENV/bin/yt" ] || { echo "Нет $VENV/bin/yt. Запусти 03-install-host-python.sh"; exit 1; }
export PATH="$VENV/bin:$PATH"

# --- Проверка биндингов ---------------------------------------------------
# yt_driver_rpc_bindings содержит driver_rpc_lib*.so — C++ Driver, который умеет
# говорить с RPC-прокси по бинарному протоколу. Без него native_driver.make_request
# упадёт на `import yt_driver_rpc_bindings` (native_driver.py:65) и YT-error будет:
#   "Driver class not found, install RPC driver bindings".
#
# ВАЖНО: ставим в тот python, который РЕАЛЬНО исполняет `yt`, а не в активный venv.
# У нас `yt` может быть установлен через pipx (`~/.local/bin/yt` с шебангом на
# `~/.local/share/pipx/venvs/ytsaurus-client/bin/python`). Тогда `source venv;
# pip install ...` положит биндинг не туда — `yt` его не увидит, и тест упадёт
# с той же ошибкой даже если `python -c "import yt_driver_rpc_bindings"` работает.
# Определяем yt-овский python через shebang и ставим биндинг прямо в него.
YT_BIN="$(command -v yt)"
YT_PY="$(head -1 "$YT_BIN" | sed -E 's|^#!\s*||')"
if [ ! -x "$YT_PY" ]; then
    # на случай если у yt не #!-launcher (например, wrapper-скрипт)
    YT_PY="$(python -c 'import sys; print(sys.executable)')"
fi
echo "yt CLI:        $YT_BIN"
echo "yt interpreter: $YT_PY"

if ! "$YT_PY" -c "from yt_driver_rpc_bindings import Driver" 2>/dev/null; then
    echo "=== Ставим ytsaurus-rpc-driver в интерпретатор yt ($YT_PY) ==="
    case "$YT_PY" in
        */pipx/venvs/ytsaurus-client/*)
            # Для pipx используем `pipx inject` — это правильный путь, он
            # регистрирует пакет в pipx-метадате (uninstall/upgrade его учтёт).
            pipx inject ytsaurus-client ytsaurus-rpc-driver
            ;;
        *)
            "$YT_PY" -m pip install --quiet ytsaurus-rpc-driver
            ;;
    esac
    "$YT_PY" -c "from yt_driver_rpc_bindings import Driver; print('OK:', Driver)"
fi

# --- Проверка наличия RPC-прокси в кластере -------------------------------
# RPC-прокси регистрируются в //sys/rpc_proxies (см. yt_env.py:929-946).
# Если список пуст — RPC-прокси не подняты и discovery вернёт пустой набор
# эндпоинтов; запрос упадёт с "no rpc proxies available".
# НЕ глотаем stderr: если `yt` падает на импорте (сломанный pipx, не та версия),
# хотим видеть стектрейс, а не загадочное «пусто → нет RPC-прокси».
rpc_proxies=$(yt list //sys/rpc_proxies)
if [ -z "$rpc_proxies" ]; then
    echo "В //sys/rpc_proxies пусто — кластер поднят без RPC-прокси."
    echo "Перезапусти кластер с --rpc-proxy-count 1 (см. комментарии в скрипте)."
    exit 1
fi
echo "=== RPC-прокси в кластере ==="
echo "$rpc_proxies"

# --- Включаем RPC backend для всех последующих yt-вызовов в этом шелле ----
# default_config.py:1341 читает YT_CONFIG_PATCHES, парсит YSON и накатывает поверх
# дефолтного конфига. Ключ верхнего уровня "backend" = "rpc" заставит
# get_backend_type вернуть "rpc" (config.py:84-95).
export YT_CONFIG_PATCHES='{backend=rpc;}'

# --- Следующий свободный номер таблицы ------------------------------------
existing=$(yt list //home 2>/dev/null | grep -E '^input_table_rpc_[0-9]+$' || true)
if [ -z "$existing" ]; then
    n=1
else
    max=$(echo "$existing" | sed 's/^input_table_rpc_//' | sort -n | tail -1)
    n=$((max + 1))
fi

table="//home/input_table_rpc_${n}"

echo
echo "=== Создаю $table (через RPC) ==="
yt create table "$table" --attributes '{schema = [{name = id; type = int64}; {name = text; type = string}]}'

echo
echo "=== Пишу в $table (через RPC) ==="
echo '{"id": 0, "text": "Hello"} {"id": 1, "text": "World!"}' \
    | yt write-table "$table" --format json

echo
echo "=== Читаю $table (через RPC) ==="
yt read-table "$table" --format json
