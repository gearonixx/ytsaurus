#!/usr/bin/env bash
# Установка Python-обвязки YTsaurus на хост в изолированный venv.
# Запускать ОДИН РАЗ.
#
# Почему именно venv:
#   На Arch (PEP 668) системный pip заблокирован — `pip install --user` бросает
#   externally-managed-environment, а `--break-system-packages` ломает Arch.
#   venv — единственный безопасный путь, ничего системного не трогает.
#
# Почему НЕ просто `pip install ytsaurus-client ytsaurus-local` с PyPI:
#   PyPI-версии (ytsaurus-client 0.13.49, ytsaurus-local 0.1.2.post0) отстают
#   от твоего trunk-бинаря (26.1.0). Конкретные симптомы:
#     * мастер падает с `Missing required parameter /primary_master/peers`
#       (старый ytsaurus-local генерит конфиг с `primary_master.addresses`,
#        новый мастер ждёт `primary_master.peers`).
#     * `from pipes import quote` в porto_helpers.py — модуль удалён в Python 3.13.
#     * Парт несовместимостей по API между client и кластером.
#   Поэтому ставим trunk-dev editable-инсталлом из исходников репо.

set -e

REPO_ROOT="${REPO_ROOT:-$HOME/try3/ytsaurus}"     # путь до твоего clone'а
VENV="${VENV:-$HOME/yt-venv}"

if [ ! -d "$REPO_ROOT" ]; then
    echo "Не вижу $REPO_ROOT. Поправь REPO_ROOT в скрипте или экспортируй переменную:"
    echo "    REPO_ROOT=/путь/к/ytsaurus $0"
    exit 1
fi

echo "=== 1. Создаём venv в $VENV ==="
python -m venv "$VENV"

echo
echo "=== 2. Активируем venv (на время этого скрипта) ==="
# shellcheck disable=SC1090
source "$VENV/bin/activate"
which pip
which python

echo
echo "=== 3. Запускаем install_locally.sh ==="
# Что он делает (расшифровка):
#   * pip install -e ytsaurus/yt/python/packages → ставит пакет yt_setup (генератор)
#   * python -m yt_setup.prepare_python_modules →
#       копирует yt/python/yt/* в /tmp/yt_tmp_build_dir
#       тащит из contrib/python/* зависимости в /tmp/yt_tmp_build_dir/yt/packages/
#       (это и есть «вендоринг» — `yt.packages.requests`, `yt.packages.decorator` и т.п.)
#   * заменяет реальные копии на симлинки на твои исходники для горячей правки:
#       yt/wrapper, yt/yson, yt/environment, yt/cli, yt/clickhouse, и др.
#   * mv в репо: ytsaurus-client-trunk-dev/build/
#   * подкладывает build_setup.py как setup.py
#   * pip install -e ytsaurus-client-trunk-dev/build → ставит editable пакет
#   * pip install ytsaurus-yson (нативные C-расширения, ставится бинарным wheel)
#
# Заметка про typo: внутри есть `echo "import linked yt.wraper - ok"` — это
# опечатка апстрима (wraper вместо wrapper), на работоспособность не влияет.
#
# Грабли «build/ из Docker»: если репо когда-то собирался в контейнере, в
# ytsaurus-client-trunk-dev/build/yt/ остаются симлинки на /workspace/... .
# На хосте они dangling, и любой второй заход без `rm -rf build/` оставит
# мёртвые симлинки. install_locally.sh снесёт всё сам (`rm -rf build/*`), но
# делаем это явно — чтобы при дебаге не возникало вопросов «откуда там
# /workspace».
BUILD_DIR="$REPO_ROOT/yt/python/packages/ytsaurus-client-trunk-dev/build"
if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi
cd "$REPO_ROOT/yt/python/packages/ytsaurus-client-trunk-dev/"
./install_locally.sh

echo
echo "=== 4. Добивка: пакеты, которых не хватает ==="
# attrs нужен потому, что вендоренный yt.packages.attr (из contrib/python/attrs/py2/)
# слишком старый — он не имеет атрибута `_next_gen`, и фоллбэк через `import attr`
# не работает без установленного пакета attrs.
pip install attrs

echo
echo "=== 5. Симлинк на CLI yt_local ==="
# install_locally.sh ставит только ytsaurus-client. CLI `yt_local` лежит в исходниках
# как обычный shebang-скрипт (yt/python/yt/local/bin/yt_local). Ставим симлинк в
# venv/bin, чтобы команда оказалась в $PATH при активации venv.
#
# ВАЖНО: не делать `pip install ytsaurus-local` с PyPI — оно поставит свою копию
# yt/local + yt/environment в site-packages, которая затенит наши editable-симлинки
# на trunk-исходники и принесёт обратно баги (pipes, primary_master.addresses).
ln -sf "$REPO_ROOT/yt/python/yt/local/bin/yt_local" "$VENV/bin/yt_local"
chmod +x "$VENV/bin/yt_local"

echo
echo "=== 6. Проверка ==="
which yt_local
yt_local --help | head -10

echo
echo "=== 7. Sanity check editable-инсталла ==="
# Главный класс ошибок, который привёл к появлению этого шага:
#   * site-packages/yt/ остался пустым каталогом → `import yt` находит его как
#     namespace package, но `yt.local` отсутствует → `yt_local` падает с
#     `ModuleNotFoundError: No module named 'yt.local'`.
#   * build/yt/<pkg> — болтающийся симлинк на /workspace/... (Docker-артефакт),
#     `import yt.wrapper` не находит файлы.
# Проверяем явно: импорт ключевых подмодулей должен вернуть пути ВНУТРИ
# build-каталога (или симлинки оттуда на репо).
EXPECTED_PREFIX="$REPO_ROOT/yt/python"
for mod in yt.local yt.wrapper yt.environment; do
    path="$(python -c "import $mod, os; print(os.path.realpath($mod.__file__))" 2>&1 || true)"
    case "$path" in
        "$EXPECTED_PREFIX"/*)
            echo "  ok   $mod -> $path"
            ;;
        *)
            echo "  FAIL $mod -> $path"
            echo "       editable-инсталл не работает. См. install_locally.sh."
            exit 1
            ;;
    esac
done

echo
echo "OK"
echo
echo "В каждой новой сессии шелла, где собираешься работать с yt_local:"
echo "    source $VENV/bin/activate"
echo
echo "Выйти из venv:"
echo "    deactivate"
