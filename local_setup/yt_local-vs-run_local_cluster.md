# `yt_local` vs `run_local_cluster.sh` — что и когда

Внутри репо есть **два разных** инструмента для поднятия локального кластера. Имена похожи, путаются — поэтому отдельно.

## TL;DR

- **`run_local_cluster.sh`** = поднимает кластер целиком из готового docker-образа `ytsaurus/local:stable` с DockerHub. Бинарь оттуда. Твою сборку не использует. UI поднимается автоматически.
- **`yt_local`** = питоновская CLI-обёртка. Запускает **твой** `ytserver-all` как обычные процессы. UI вручную отдельным контейнером.

## Чем они принципиально разные

| | `run_local_cluster.sh` | `yt_local` |
|---|---|---|
| Где лежит | `yt/docker/local/run_local_cluster.sh` | `yt/python/yt/local/bin/yt_local` |
| Что это | Bash-обёртка над `docker run` | Python-CLI |
| Откуда берёт `ytserver-all` | Из docker-образа `ytsaurus/local:stable` (DockerHub) | Из `--ytserver-all-path` — ты явно указываешь |
| Подменить на свой билд | **Нельзя** (только пересобирать сам образ) | **Да** — это и есть смысл |
| Что ещё поднимает | UI (`ytsaurus/ui:stable`), Prometheus, docker-сеть `yt_local_cluster_network` | Только сам кластер: master, node, scheduler, http_proxy, controller_agent |
| Изоляция | Всё в docker-контейнерах | Голые процессы на хосте (или внутри контейнера) |
| Целевая аудитория | Пользователи/прикладники: «дайте мне YTsaurus, я буду писать клиентский код» | Разработчики самого YT: «я правлю серверный код и хочу проверить» |
| Если поправил `yt/yt/server/http_proxy/context.cpp` | **Ничего не увидишь.** Бинарь из docker-образа, твоих правок там нет | **Увидишь** — после `ninja` и `docker cp` |

## Команды, чтоб не путать

### `run_local_cluster.sh` (когда нужен «готовый YT для пощупать»)

```
bash /workspace/ytsaurus/yt/docker/local/run_local_cluster.sh \
    --proxy-port 8000 --interface-port 8001
```

Через пару минут:
- HTTP API: http://localhost:8000
- UI: http://localhost:8001

Остановить:
```
bash /workspace/ytsaurus/yt/docker/local/run_local_cluster.sh --stop
```

### `yt_local` (твой кейс — гоняем свой бинарь)

См. `05-start-cluster.sh` рядом. Кратко:
```
source ~/yt-venv/bin/activate
ulimit -n 524288
yt_local start \
    --enable-debug-logging \
    --proxy-port 8000 --fqdn localhost \
    --ytserver-all-path /tmp/ytserver-all \
    --sync
```

UI поднимаешь отдельно — см. `06-start-ui.sh`.

## Когда что использовать

- Если хочешь убедиться, что **сам YT работает**, и заняться чем-то поверх (питон-клиент, проба запросов) → `run_local_cluster.sh`.
- Если правишь C++-код сервера и тебе важно увидеть эффект своих правок в живом кластере → `yt_local`.
- Если хочется UI на нестандартной сборке (например, твой бинарь + UI) → `yt_local` + `06-start-ui.sh`.
