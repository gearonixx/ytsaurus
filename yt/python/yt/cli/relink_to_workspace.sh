#!/bin/bash
# Перенаправить подпакеты установленного `yt` на это рабочее дерево, чтобы
# `yt` CLI исполнял код отсюда (live-edit без rebuild / без `pip install -e`).
#
# Зачем
# -----
# `yt` обычно поставлен через pipx (`~/.local/bin/yt`), его шебанг указывает
# на pipx-venv. Этот venv держит обычную (не editable) копию пакета `yt/`
# в site-packages. Любые правки в этом дереве (например print/log в
# yt/wrapper/default_config.py) не подхватываются — CLI грузит чужую копию.
#
# Решение: заменить подпапки `yt/<pkg>` в site-packages симлинками на
# соответствующие подпапки этого репо. Откат — флаг `--unlink`.
#
# Использование
# -------------
#   ./relink_to_workspace.sh              # включить симлинки
#   ./relink_to_workspace.sh --unlink     # вернуть оригиналы
#   YT_PKG_DIR=/path/to/yt ./relink...    # переопределить целевой пакет
#
# Что определяется автоматически
# ------------------------------
#   * Целевой `yt/` находится из шебанга `yt` CLI:
#       `$YT_PY -c "import yt; print(yt.__path__[0])"`.
#   * Источник — каталог `..` относительно этого скрипта (yt/python/yt).
#   * Связываются ТОЛЬКО подпакеты, существующие и в источнике, и в цели.
#     Остальное (admin/, packages/, …) остаётся нетронутым.
#
# Бэкап
# -----
# При первой линковке оригинальный каталог переименовывается в `<pkg>.orig`.
# `--unlink` снимает симлинк и возвращает `<pkg>.orig` обратно.

set -euo pipefail

ACTION="link"
if [ "${1:-}" = "--unlink" ]; then
    ACTION="unlink"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$SCRIPT_DIR/.." && pwd)"   # /…/ytsaurus/yt/python/yt

# Цель: либо явный YT_PKG_DIR, либо из python, который исполняет `yt`.
if [ -n "${YT_PKG_DIR:-}" ]; then
    DST="$YT_PKG_DIR"
else
    YT_BIN="$(command -v yt || true)"
    if [ -z "$YT_BIN" ]; then
        echo "yt CLI не в PATH. Поставь его (pipx install ytsaurus-client) или укажи YT_PKG_DIR." >&2
        exit 1
    fi
    YT_PY="$(head -1 "$YT_BIN" | sed -E 's|^#!\s*||')"
    if [ ! -x "$YT_PY" ]; then
        echo "Не удалось распарсить интерпретатор yt из $YT_BIN (shebang: $YT_PY)." >&2
        exit 1
    fi
    DST="$("$YT_PY" -c "import yt, os; print(os.path.dirname(yt.__file__))")"
fi

if [ ! -d "$DST" ]; then
    echo "Целевой каталог не существует: $DST" >&2
    exit 1
fi
if [ ! -d "$SRC" ]; then
    echo "Каталог исходников не существует: $SRC" >&2
    exit 1
fi

echo "SRC: $SRC"
echo "DST: $DST"
echo "Action: $ACTION"
echo

linked=0
restored=0
skipped=0

# Берём пересечение подпакетов SRC ∩ DST. Файлы (.py) не трогаем — только пакеты.
for src_path in "$SRC"/*/; do
    pkg="$(basename "$src_path")"
    dst_path="$DST/$pkg"
    backup_path="$DST/${pkg}.orig"

    case "$ACTION" in
        link)
            if [ ! -e "$dst_path" ] && [ ! -L "$dst_path" ]; then
                echo "skip $pkg (нет в цели)"
                skipped=$((skipped+1))
                continue
            fi
            if [ -L "$dst_path" ]; then
                current="$(readlink "$dst_path")"
                if [ "$current" = "$src_path" ] || [ "$current" = "${src_path%/}" ]; then
                    echo "ok   $pkg (уже линкуется на $current)"
                    continue
                fi
                echo "warn $pkg: уже симлинк на $current, перезаписываю"
                rm "$dst_path"
            elif [ -d "$dst_path" ] && [ ! -e "$backup_path" ]; then
                mv "$dst_path" "$backup_path"
                echo "backup $pkg -> ${pkg}.orig"
            elif [ -d "$dst_path" ] && [ -e "$backup_path" ]; then
                # Бэкап уже был — текущий каталог странный, не трогаем.
                echo "warn $pkg: и каталог, и ${pkg}.orig существуют; пропускаю"
                skipped=$((skipped+1))
                continue
            fi
            ln -snf "${src_path%/}" "$dst_path"
            echo "link $dst_path -> ${src_path%/}"
            linked=$((linked+1))
            ;;
        unlink)
            if [ -L "$dst_path" ]; then
                rm "$dst_path"
                if [ -d "$backup_path" ]; then
                    mv "$backup_path" "$dst_path"
                    echo "restore $pkg (из ${pkg}.orig)"
                    restored=$((restored+1))
                else
                    echo "warn $pkg: убрал симлинк, но ${pkg}.orig нет — цели без оригинала"
                fi
            fi
            ;;
    esac
done

echo
case "$ACTION" in
    link)
        echo "linked: $linked, skipped: $skipped"
        echo
        echo "Проверка:"
        echo "  $(command -v yt) -c 'import yt.wrapper.default_config as m; print(m.__file__)'"
        echo "  (должен указывать в $SRC/wrapper/default_config.py)"
        ;;
    unlink)
        echo "restored: $restored"
        ;;
esac
