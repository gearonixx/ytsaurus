# Полная история — как мы пришли от пустого хоста до работающего YT с UI и debug-логами

Транскрипт разговора, восстановленный по ходу. Все тупики и решения хронологически — чтобы будущий ты или коллега не повторял путь, а видел сразу, где грабли.

---

## Стартовая точка

Был docker-контейнер `ytsaurus-dev` (image `ytsaurus-build`), внутри уже собранный YT через `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ...` и `ninja`. Полный билд = 12242 целей, занимал часы. Для запуска кластера нужны только два: `yt-server-http_proxy` и `ytserver-all`.

```
docker exec -it ytsaurus-dev bash
cd /workspace/build
ninja yt-server-http_proxy ytserver-all   # 1882 цели, минуты
```

**Грабля №1:** прерывание ninja (`Ctrl+C`) ничего не ломает — `.o`-файлы остаются, при следующем запуске ninja докомпиливает только то, что не дошло. Голый `ninja` без аргументов строит ВСЁ (включая тесты и утилиты, которые для запуска кластера не нужны).

---

## Первая попытка запуска — yt_local внутри контейнера

```
mkdir -p /tmp/yt_local && cd /tmp/yt_local
export PYTHONPATH=/workspace/ytsaurus/yt/python
python3 /workspace/ytsaurus/yt/python/yt/local/bin/yt_local start \
    --proxy-port 8000 --fqdn localhost \
    --ytserver-all-path /workspace/build/yt/yt/server/all/ytserver-all \
    --sync
```

**`PYTHONPATH=…/yt/python`** нужен, потому что `yt_local` импортирует `yt.wrapper`, `yt.environment` и пр. — их в системе нет, есть только в дереве репо.

Сразу серия ошибок `ModuleNotFoundError`:

1. `simplejson` → `pip3 install --user simplejson`
2. `dacite` → `pip3 install --user dacite`
3. `decorator` → `pip3 install --user decorator`
4. `yt.packages.requests` → **новый класс проблемы**

`yt.packages.requests` физически в репо **отсутствует**. В `yt/python/yt/packages/` только `__init__.py`, `expiringdict.py`, `importlib.py`. Реальные «вендоренные» пакеты собираются скриптом `yt_setup.prepare_python_modules` в момент сборки wheel'ы.

**Решение:** запустить готовый скрипт `install_locally.sh`, который через `yt_setup.prepare_python_modules` собирает правильное дерево и ставит trunk-питон через `pip install -e`:

```
bash /workspace/ytsaurus/yt/python/packages/ytsaurus-client-trunk-dev/install_locally.sh
```

После него `import yt.wrapper` работает. Этот же скрипт ставит `ytsaurus-yson` (там C-extensions, поэтому из готового wheel).

Но `ytsaurus-local` (где сам CLI `yt_local`) `install_locally.sh` не ставит — пришлось руками:
```
pip3 install --user --no-deps ytsaurus-local
ln -sf /workspace/ytsaurus/yt/python/yt/local/bin/yt_local "$HOME/.local/bin/yt_local"
```

---

## Конфигурационная несовместимость PyPI ↔ trunk

Кластер запускается, мастер мгновенно падает:
```
Error loading config file …/master-0-0.yson
  Error loading parameter /primary_master
    Missing required parameter /primary_master/peers
```

Старая PyPI'шная `ytsaurus-local 0.1.2.post0` генерит конфиг с `primary_master.addresses = [...]` (старый формат, для YT 25.x). Наш trunk-бинарь 26.1.0 ждёт `primary_master.peers = [{...}]`.

**Решение:** установить и `ytsaurus-local` через trunk — снять PyPI и не возвращать его.

---

## Главная боль: падение через ~28-30с с OOM

Кластер успешно поднимался, в логах `Local YT started, HTTP proxy addresses: ['localhost:8000']`. Через ~28с все процессы умирают одновременно с одинаковым crash dump:

```
*** Aborted at ...
SIGSEGV (Segmentation violation) (@0x0) received by PID … code SEGV_MAPERR
RAX=0  R14=0x2492492492492493
*** Out-of-memory during object allocation
*** Aborting process with exit code 9 (OutOfMemory)
```

`R14 = 0x2492492492492493` — libdivide-константа для div-by-7. Это TCMalloc fast-path size-class lookup.

`AbortOnOom` из `library/cpp/yt/memory/new.cpp:13` срабатывает буквально когда `malloc()` вернул `nullptr`.

### Что перепробовали (всё **не** помогло сразу)

| Гипотеза | Команда | Эффект |
|---|---|---|
| Системная память исчерпана | `free -h`, `cat /sys/fs/cgroup/memory.max` | 30Gi RAM, лимита нет — отвергнуто |
| `vm.overcommit_memory=0` блокирует резервации | `sudo sysctl -w vm.overcommit_memory=1` (на хосте) | Помогло пройти стартовую фазу |
| `vm.max_map_count=65536` исчерпывается | `sudo sysctl -w vm.max_map_count=1048576` | Без эффекта, у нас было ~600 mmap-регионов |
| `ulimit -n 1024` — слишком мало FD | `ulimit -n 524288` | Без эффекта на 30-секундный таймер |
| Hard memory limit от YT TCMalloc manager | Прочитали `tcmalloc_manager.cpp`, увидели `SetMemoryLimit(..., kHard)` — но в cgroup v2 это не активируется | Отвергнуто |
| Hugepage-aware allocator | `TCMALLOC_HUGEPAGE_AWARE_ALLOCATOR=false` | Без эффекта |

### Финальный диагноз

`/proc/self/status` показал `Seccomp: 2, Seccomp_filters: 1` — Docker применяет default seccomp profile. Этот фильтр блокирует часть NUMA/memory syscalls (`mbind`, `set_mempolicy`, `migrate_pages`).

TCMalloc периодически (фоновый release thread, ~30с) пытается их вызвать → `EPERM` → кривое состояние → следующий `malloc()` возвращает `nullptr` → `AbortOnOom`.

### Решение

Два варианта:
1. Пересоздать docker-контейнер с `--security-opt seccomp=unconfined --security-opt apparmor=unconfined --cap-add SYS_PTRACE --cap-add SYS_ADMIN`
2. **Запускать `yt_local` нативно на хосте**, а контейнер использовать только для сборки

Выбран **вариант 2** — меньше телодвижений, безопаснее в долгую.

---

## Перенос на хост

```
# 1. Достаём бинарь из контейнера (на хосте)
docker cp ytsaurus-dev:/workspace/build/yt/yt/server/all/ytserver-all /tmp/ytserver-all
chmod +x /tmp/ytserver-all
/tmp/ytserver-all --version   # 26.1.0-local-os~b5a39533962b7a01+distbuild — ОК
```

Arch glibc новее, чем в контейнере — бинарь запустился.

### Python на хосте: PEP 668

```
pip install --user ytsaurus-client → error: externally-managed-environment
```

Arch блокирует системный pip с подачи PEP 668. Решение — venv:

```
python -m venv ~/yt-venv
source ~/yt-venv/bin/activate
cd ~/try3/ytsaurus/yt/python/packages/ytsaurus-client-trunk-dev/
./install_locally.sh
```

Прокатило успешно. Дальше:
```
pip install --no-deps ytsaurus-local       # ОШИБКА — см. ниже
```

### Грабля: PyPI ytsaurus-local затеняет trunk

PyPI'шный `ytsaurus-local 0.1.2.post0` поставил свои собственные `yt/local/` и `yt/environment/` в venv site-packages. Они **тенят** trunk-симлинки.

Импорт `from pipes import quote` упал — модуль `pipes` удалён из stdlib в Python 3.13+, а у нас Python 3.14 на Arch.

```
pip uninstall -y ytsaurus-local
```

После этого `yt.local` стал тянуться из trunk-исходников (через editable `ytsaurus-client-trunk-dev`).

### Грабля: `yt.packages.attr._next_gen`

```
ModuleNotFoundError: No module named 'yt.packages.attr._next_gen'
```

Вендоренный `contrib/python/attrs/py2/attr` — это **py2-версия** attrs, у неё нет `_next_gen` (это атрибут современного attrs 20+). Фоллбэк `import attr` тоже не работал — в venv ничего нет.

```
pip install attrs
```

### Грабля: симлинка yt_local CLI

После uninstall PyPI ytsaurus-local исчез и `~/yt-venv/bin/yt_local`. Нужно поставить руками:
```
ln -sf ~/try3/ytsaurus/yt/python/yt/local/bin/yt_local ~/yt-venv/bin/yt_local
chmod +x ~/yt-venv/bin/yt_local
```

### Запуск

```
source ~/yt-venv/bin/activate
ulimit -n 524288                  # с FD-лимитом 1024 рано или поздно EMFILE
mkdir -p /tmp/yt_local && cd /tmp/yt_local
yt_local start \
    --proxy-port 8000 --fqdn localhost \
    --ytserver-all-path /tmp/ytserver-all \
    --sync
```

**Кластер встал и не падает.** Проверка:
```
curl -s 'http://localhost:8000/api/v4/get?path=//sys/@cluster_name'
# {"value":"a7ca8a4a-786ee42-9b17270-87bb7178"}
```

---

## UI

`yt_local` поднимает только бэкенд (master + node + scheduler + http_proxy + controller_agent). UI — отдельным docker-контейнером:

```
docker run -d --name yt-ui \
    --add-host=host.docker.internal:host-gateway \
    -p 8001:80 \
    -e YT_LOCAL_CLUSTER_ID=local \
    -e PROXY=localhost:8000 \
    -e PROXY_INTERNAL=host.docker.internal:8000 \
    -e APP_ENV=local \
    -e APP_INSTALLATION=custom \
    -e ALLOW_PASSWORD_AUTH=1 \
    ghcr.io/ytsaurus/ui:stable
```

**Ключевая тонкость:** UI внутри docker, кластер на хосте. `PROXY=localhost:8000` — что UI **показывает в браузере** (браузер живёт на хосте, для него localhost — это кластер). `PROXY_INTERNAL=host.docker.internal:8000` — что UI-сервер использует **изнутри контейнера**, чтобы достучаться до хост-кластера. На Linux `host.docker.internal` появляется через `--add-host=...:host-gateway`.

`http://localhost:8001` — UI. Логин `root` без пароля.

---

## Логи: где и как смотреть

```
/tmp/yt_local/<id>/logs/
    http-proxy-0.log              INFO+, текст
    http-proxy-0.debug.log        DEBUG+, текст (только если --enable-debug-logging)
    http-proxy-0.json.log         JSON-структура (только если --enable-structured-logging)
    master-0-0.{log,debug.log,json.log}
    node-0.{log,debug.log,json.log}
    scheduler-0.{log,debug.log,json.log}
    controller-agent-0.{log,debug.log,json.log}
    watcher.log
```

**Сюрприз №1:** по умолчанию debug-логи пишутся **только** для controller-agent. У всех остальных — обрезано на info, и `YT_LOG_DEBUG` из http_proxy не попадает никуда. Лечится:
```
yt_local start --enable-debug-logging ...
```

**Сюрприз №2:** есть структурированные JSON-логи, в которые `TErrorAttribute(...)` прилетают как отдельные поля (а не сваливаются в плоский текст). Включается:
```
yt_local start --enable-structured-logging ...
```

И тогда `jq` становится главным инструментом:
```
jq 'select(.level=="E")' /tmp/yt_local/*/logs/http-proxy-0.json.log
```

---

## Полный цикл «изменил код → увидел лог»

После добавления `YT_LOG_DEBUG(...)` в `yt/yt/server/http_proxy/context.cpp`:

```bash
# 1. В контейнере — пересборка
docker exec ytsaurus-dev bash -c 'cd /workspace/build && ninja yt-server-http_proxy ytserver-all'

# 2. На хосте — обновить копию бинаря
docker cp ytsaurus-dev:/workspace/build/yt/yt/server/all/ytserver-all /tmp/ytserver-all

# 3. Перезапустить кластер
# (Ctrl+C на yt_local --sync, или yt_local stop <id>)
yt_local start --enable-debug-logging --enable-structured-logging \
    --proxy-port 8000 --fqdn localhost \
    --ytserver-all-path /tmp/ytserver-all --sync

# 4. Триггер
curl -s 'http://localhost:8000/api/v4/get?path=//sys/@cluster_name'

# 5. Просмотр
grep 'Running driver request' /tmp/yt_local/*/logs/http-proxy-0.debug.log | tail
# или то же в JSON:
jq -c 'select(.message | startswith("Running driver request"))' \
   /tmp/yt_local/*/logs/http-proxy-0.json.log
```

И в итоге увидели:
```
Running driver request (Id: 82307f37-15a1acfe-569521c3-10d68f2b, CommandName: read_table,
  AuthenticatedUser: root, UserTag: <null>, UserRemoteAddress: tcp://[::1]:60036,
  UserTokenPresent: False, ..., Parameters: {"input_format"="yson"; ...; 
  "transaction_id"="1-1ada-10001-10a7"; ...}, InputStreamPresent: True, ...)
```

---

## Концептуальные моменты, которые всплыли по дороге

### yt_local vs run_local_cluster.sh
- **`run_local_cluster.sh`** — bash-обёртка над `docker run`, тянет образ `ytsaurus/local:stable` с DockerHub, поднимает кластер целиком, плюс UI, плюс Prometheus. Твою сборку **не использует**. Для пользователей.
- **`yt_local`** — Python-CLI. Запускает **твой** `ytserver-all`. UI вручную. Для разработчиков самого YT.

### Driver request vs Transaction
Один `Running driver request (Id: …)` в логе = **один HTTP-запрос**, не транзакция. Транзакция — это контекст, который объединяет много driver request'ов между `start_transaction` и `commit_transaction`. В логе у запроса виден `transaction_id` внутри `Parameters` — это идентификатор той транзакции, в рамках которой запрос выполняется.

Один запрос **может быть и без транзакции** (или с нулевой `0-0-0-0`) — это случай простых cypress-операций типа `get //sys/...`.

### Каналы логов и ошибок
Кроме `.log` есть:
- **`X-YT-Error` HTTP-заголовок в ответе** — для отладки клиентских ошибок файлы вообще не нужны.
- **Orchid** (`/api/v4/get?path=//sys/http_proxies/.../orchid/...`) — live-интроспекция счётчиков и состояния, без ожидания триггера.
- **Prometheus** на `/solomon/all` или `/metrics` — графики и алерты.
- **Trace ID grep** — корреляция одного запроса по всем компонентам.

Подробнее в `logs-and-errors.md`.

---

## Финальные артефакты

```
local_setup/
    README.md                          точка входа
    journey.md                         этот файл — полная история со всеми граблями
    yt_local-vs-run_local_cluster.md   разница между двумя инструментами
    logs-and-errors.md                 5 каналов наблюдения логов и ошибок
    troubleshooting.md                 15+ конкретных грабель с симптомами и фиксами
    01-build-binary.sh                 в контейнере: cmake + ninja
    02-prepare-host.sh                 хост: sysctl + /etc/sysctl.d/99-yt.conf
    03-install-host-python.sh          хост: venv + install_locally + attrs + симлинк
    04-copy-binary.sh                  docker cp
    05-start-cluster.sh                yt_local start с debug + structured logging
    06-start-ui.sh                     docker run ytsaurus/ui:stable
    07-tail-logs.sh                    tail -f + jq-рецепты
    08-stop-all.sh                     остановка всего
```

Total time on the path: несколько часов отладки в две стороны. С этим документом — пол часа от пустого хоста до работающего UI с твоим бинарём.
