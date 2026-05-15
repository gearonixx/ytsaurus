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
echo "OK"
echo
echo "В каждой новой сессии шелла, где собираешься работать с yt_local:"
echo "    source $VENV/bin/activate"
echo
echo "Выйти из venv:"
echo "    deactivate"
