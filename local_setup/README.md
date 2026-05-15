# local_setup — запуск локального YTsaurus-кластера со своим бинарём

Скрипты и заметки про то, как поднять собственный YTsaurus-кластер из свежесобранного `ytserver-all` и подцепить к нему web UI.

Бинарь собирается **в docker-контейнере** (там уже стоит весь toolchain), а кластер запускается **на хосте**. Так — потому что внутри контейнера дефолтный seccomp-профиль Docker блокирует часть memory-related syscalls (NUMA, hugepage hint'ы), без которых tcmalloc внутри YT-процессов падает с `OutOfMemory` через ~30 секунд после старта.

## yt_local vs run_local_cluster.sh

Два **разных** инструмента в репо. Полное сравнение — в [`yt_local-vs-run_local_cluster.md`](yt_local-vs-run_local_cluster.md). Кратко:

- **`run_local_cluster.sh`** — поднимает кластер из DockerHub-образа `ytsaurus/local:stable`. Бинарь оттуда. Твою сборку **не использует**. UI стартует вместе с кластером.
- **`yt_local`** (этот гайд) — питоновская CLI. Запускает **твой** `ytserver-all` нативными процессами. UI подключается вручную.

## Порядок прохождения

| Шаг | Где запускать | Скрипт |
|---|---|---|
| 1. Собрать бинарь | внутри контейнера `ytsaurus-dev` | [`01-build-binary.sh`](01-build-binary.sh) |
| 2. Подготовить хост (один раз) | на хосте | [`02-prepare-host.sh`](02-prepare-host.sh) |
| 3. Поставить Python-обвязку (один раз) | на хосте | [`03-install-host-python.sh`](03-install-host-python.sh) |
| 4. Скопировать бинарь к себе | на хосте | [`04-copy-binary.sh`](04-copy-binary.sh) |
| 5. Запустить кластер | на хосте | [`05-start-cluster.sh`](05-start-cluster.sh) |
| 6. Поднять UI | на хосте | [`06-start-ui.sh`](06-start-ui.sh) |
| 7. Смотреть логи | на хосте | [`07-tail-logs.sh`](07-tail-logs.sh) |
| 8. Остановить всё | на хосте | [`08-stop-all.sh`](08-stop-all.sh) |

После любой правки в серверном коде (например `yt/yt/server/http_proxy/context.cpp`) — заново 1 → 4 → 5.

После любой правки в Python-коде в `yt/python/` — ничего пересобирать не нужно, исходники подключены editable'ом.

## Что в репозитории нужно знать

- `yt/yt/server/all/` — целевой бинарь `ytserver-all`, multi-call, внутри все компоненты.
- `yt/python/yt/local/bin/yt_local` — CLI, который мы запускаем.
- `yt/python/yt/environment/` — генератор YT-конфигов под локальный кластер.
- `yt/python/packages/ytsaurus-client-trunk-dev/install_locally.sh` — официальный скрипт установки питон-клиента из текущего trunk'а в editable-режиме.

## Логи и ошибки — что смотреть кроме `.log`

См. [`logs-and-errors.md`](logs-and-errors.md). Кратко: текстовые файлы — низший слой. Над ними есть структурированные JSON-логи (`--enable-structured-logging`), HTTP-ответ с `X-YT-Error`, live-orchid и Prometheus.

## Известные грабли

См. [`troubleshooting.md`](troubleshooting.md).
