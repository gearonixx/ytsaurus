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

# Аутентификация на HTTP-прокси.
#   ENABLE_AUTH=1 (по умолчанию) -> кластер поднимается с
#     --enable-auth --native-client-supported --create-admin-user.
#   ENABLE_AUTH=0 -> старый режим (root, пустой пароль, доступ без токена).
#
# Что включает --enable-auth (см. configs_provider.py:1441):
#   на HTTP-прокси выставляются auth/enable_authentication и
#   auth/require_authentication -> анонимный доступ к :8000 запрещён, нужен токен
#   (cypress_token_authenticator) либо логин по паролю через /login/ (UI).
# RPC-прокси при этом ОСТАётся открытым (configs_provider.py:1621 жёстко ставит
#   enable_authentication=false для rpc-proxy) — поэтому RPC-смоук в 11 работает
#   без токена.
# --native-client-supported обязателен: при require_authentication внутренние
#   клиенты yt_local (init world, create_admin_user) не могут ходить через
#   закрытый HTTP-прокси и используют НАТИВНЫЙ драйвер (прямое подключение к
#   мастеру в обход прокси). Требует пакет yt_driver_bindings в venv
#   (driver_lib.so из контейнера ytsaurus-dev — см. README/03).
# --create-admin-user создаёт суперюзера `admin` с cypress-токеном "password" и
#   паролем "password" (yt_env.py:774). Это bootstrap-учётка: без неё после
#   включения require_authentication войти было бы некому (chicken-and-egg).
ENABLE_AUTH="${ENABLE_AUTH:-1}"
# Абсолютный путь к дельте конфига HTTP-прокси (скрипт ниже делает cd в WORKDIR,
# поэтому относительный путь yt_local не нашёл бы).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AUTH_PROXY_DELTA="${AUTH_PROXY_DELTA:-$SCRIPT_DIR/http-proxy-auth-delta.yson}"
AUTH_ARGS=()
if [ "$ENABLE_AUTH" = "1" ]; then
    AUTH_ARGS=(--enable-auth --native-client-supported --create-admin-user)
    # Дельта добавляет auth/cypress_cookie_manager -> регистрируется /login/
    # (вход по паролю для UI). Без неё UI получает 404. См. сам файл.
    if [ -f "$AUTH_PROXY_DELTA" ]; then
        AUTH_ARGS+=(--proxy-config-path "$AUTH_PROXY_DELTA")
    else
        echo "WARN: нет $AUTH_PROXY_DELTA — /login/ не зарегистрируется, UI-вход даст 404."
    fi
fi

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

# При ENABLE_AUTH нужен нативный драйвер (yt_driver_bindings). В этом venv он не
# ставится из PyPI (пакета ytsaurus-native-driver там нет) — driver_lib.so взят
# из контейнера ytsaurus-dev и положен в site-packages вручную (см. README).
# Падаем заранее с понятной инструкцией, а не через минуту в create_admin_user.
if [ "$ENABLE_AUTH" = "1" ] && ! python -c "import yt_driver_bindings" 2>/dev/null; then
    echo "ENABLE_AUTH=1, но import yt_driver_bindings упал — нативный драйвер не установлен."
    echo "Поставь его (из контейнера ytsaurus-dev):"
    echo "  SP=\$(python -c 'import site; print(site.getsitepackages()[0])')"
    echo "  mkdir -p \"\$SP/yt_driver_bindings\""
    echo "  cp \$REPO/yt/yt/python/yt_driver_bindings/{__init__.py,driver.py} \"\$SP/yt_driver_bindings/\""
    echo "  docker cp ytsaurus-dev:/workspace/build/yt/yt/python/driver/native_shared/libdriver_lib.so \"\$SP/yt_driver_bindings/driver_lib.so\""
    echo "Либо отключи аутентификацию: ENABLE_AUTH=0 ./05-start-cluster.sh"
    exit 1
fi

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
[ "$ENABLE_AUTH" = "1" ] && echo "=== аутентификация ВКЛЮЧЕНА (admin/password; токен 'password') ===" \
                         || echo "=== аутентификация выключена (root, без токена) ==="
exec yt_local start \
    --enable-debug-logging \
    --enable-structured-logging \
    --proxy-port "$PROXY_PORT" \
    --fqdn "$FQDN" \
    --rpc-proxy-count "$RPC_PROXY_COUNT" \
    --ytserver-all-path "$YTSERVER_ALL" \
    "${AUTH_ARGS[@]}" \
    $EXTRA_YT_LOCAL_ARGS \
    --sync

# После старта смотри лог в этом же терминале (он будет показывать INFO-сообщения),
# а dump'ы компонентов — в другом терминале через 07-tail-logs.sh.
