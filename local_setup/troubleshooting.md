# Troubleshooting — всё, на что мы напоролись

Хронология реальных тупиков. Если кто-то полезет повторять путь — сюда смотрит, и часами тыкать sysctl'ы не придётся.

## 1. `cd: bluid: No such file or directory`

Опечатка, должно быть `cd build`. Безобидно, но сэкономит секунду.

## 2. yt_local внутри контейнера — `ModuleNotFoundError: No module named 'simplejson'`, потом `dacite`

`yt_local` — это Python-обёртка, у неё есть рантайм-зависимости. Первый набор:
```
pip3 install --user simplejson dacite
```

Дальше всё равно вылазит `decorator`, `six`, `attrs==22.2.0`, `tqdm`, `argcomplete`, `charset-normalizer`, `typing-extensions`, `distro` — это полный список из `yt/python/packages/ytsaurus-client/setup.py`.

## 3. `ModuleNotFoundError: No module named 'yt.packages.requests'`

`yt.packages.*` — это **«вендоренные»** пакеты, которые **физически отсутствуют** в сорс-дереве `yt/python/yt/packages/`. Там только `__init__.py`, `expiringdict.py`, `importlib.py`. Реальные `requests`, `decorator` и пр. собираются туда **скриптом** `yt_setup.prepare_python_modules` в момент сборки wheel'ы.

Решение — собрать питон-обвязку из исходников через готовый скрипт:
```
bash /workspace/ytsaurus/yt/python/packages/ytsaurus-client-trunk-dev/install_locally.sh
```

(Это и делает `03-install-host-python.sh`, но на хосте.)

## 4. `Missing required parameter /primary_master/peers`

Падает master при старте.

Причина: версия `ytsaurus-local` с PyPI (0.1.2.post0) генерит конфиг с `primary_master.addresses = [...]` — это **старый формат**, который понимал YT 25.x. Trunk-бинарь (26.1.0) ждёт новый формат: `primary_master.peers = [{...}]`.

Лечится **только** установкой trunk-питона из репо. См. п.3.

## 5. `ModuleNotFoundError: No module named 'pipes'`

Стрельнуло на хосте с Python 3.13/3.14. Модуль `pipes` удалён из стандартной библиотеки начиная с 3.13. В коде `yt/environment/porto_helpers.py` использовался `from pipes import quote`.

В trunk-исходниках уже починено (там `from shlex import quote`). Виновата опять PyPI-версия `ytsaurus-local`. Снимать:
```
pip uninstall -y ytsaurus-local
```
И не ставить с PyPI — делать симлинк на `yt/python/yt/local/bin/yt_local` (так и делает `03-install-host-python.sh`).

## 6. `ModuleNotFoundError: No module named 'yt.packages.attr._next_gen'`

Вендоренный `contrib/python/attrs/py2/attr` — это **py2-вариант** библиотеки attrs. У него нет `_next_gen` (это атрибут современного attrs 20+). Код пробует фоллбэк `import attr`, но в venv ничего нет — снова падает.

Решение:
```
pip install attrs
```

## 7. Кластер успешно стартует и **падает через 28-30 секунд** с `*** Out-of-memory during object allocation`

Это была самая болезненная часть. Симптомы:
- `Local YT started` в логах yt_local
- Через ~28с http_proxy и master умирают одновременно
- В stderr crash dump с SIGSEGV в TCMalloc fast-path (`R14=0x2492492492492493` — libdivide-константа для div-by-7, признак TCMalloc size-class lookup)
- Идентичный таймер ~30с между запусками

Перебрали (всё **не помогло**, оставлено для истории):
- `vm.overcommit_memory=1` — помог пройти стартовую фазу, но падение остаётся
- `vm.max_map_count=1048576` — без эффекта, у нас было ~600 mmap-регионов
- `ulimit -n 524288` — без эффекта на 30-секундный таймер
- TCMALLOC_HUGEPAGE_AWARE_ALLOCATOR=false — без эффекта
- Подкручивание лимитов через `--master-config-path` — без эффекта

**Реальная причина**: Docker применяет **seccomp-фильтр** (`Seccomp: 2, Seccomp_filters: 1`), который блокирует часть NUMA/memory syscalls (`mbind`, `set_mempolicy`, `migrate_pages`, иногда `userfaultfd`, `madvise(MADV_*)`). TCMalloc периодически (раз в ~30с — это фоновый release thread) пытается их использовать, ловит EPERM, оказывается в кривом состоянии → следующий `malloc()` возвращает `nullptr` → `AbortOnOom`.

**Решение**: либо пересоздать контейнер с `--security-opt seccomp=unconfined`, либо (что мы и сделали) **запускать yt_local на хосте**, а в контейнере оставить только сборку.

## 8. `Read-only file system` при `sysctl -w` внутри контейнера

`/proc/sys/vm/...` внутри Docker примонтирован read-only. Это нормально.

`vm.overcommit_memory` и `vm.max_map_count` — **глобальные ядерные настройки**, не namespace'нутся (за редкими исключениями в очень свежих ядрах). Меняешь **на хосте**:
```
sudo sysctl -w vm.overcommit_memory=1
sudo sysctl -w vm.max_map_count=1048576
```
Внутри контейнера сразу появится новое значение, рестартить контейнер не надо.

## 9. UI в Docker не может пробиться к кластеру на хосте

Симптом: UI открывается, но «Connection failed» / «503 / 502».

Причина: внутри UI-контейнера `localhost` — это сам контейнер, а не хост. У `PROXY_INTERNAL=localhost:8000` UI ходит сам к себе.

Решение:
```
docker run ... \
    --add-host=host.docker.internal:host-gateway \
    -e PROXY_INTERNAL=host.docker.internal:8000 \
    ...
```
На macOS/Windows host.docker.internal встроен. На Linux нужно `--add-host=host.docker.internal:host-gateway`.

При этом `PROXY` (без INTERNAL) пусть остаётся `localhost:8000` — он **показывается пользователю в браузере**, и для браузера, который живёт на хосте, `localhost:8000` это и есть кластер.

## 10. `debug.log` пустой / отсутствует для http_proxy, хотя в коде стоит `YT_LOG_DEBUG`

По умолчанию `yt_local` включает debug-логи только для `controller_agent` (видно по `ls /tmp/yt_local/*/logs/` — единственный с `.debug.log`).

Включается флагом:
```
yt_local start --enable-debug-logging ...
```
(Помечен deprecated в пользу `--log-level debug`, но пока работает.)

После этого появятся `http-proxy-0.debug.log`, `master-0-0.debug.log` и др.

## 11. Логи в `tail -f` не появляются после API-запроса

Проверка:
1. Запрос реально дошёл? `curl -v http://localhost:8000/...` должен вернуть 200.
2. Бинарь — действительно свежий? `/tmp/ytserver-all` был скопирован **после** правки `context.cpp`? Стандартная ошибка: правишь код, забываешь пересобрать, забываешь `docker cp` — и удивляешься.
3. Кластер был перезапущен после `docker cp`? yt_local держит уже запущенные процессы в памяти, новый бинарь подцепит только при следующем `yt_local start`.

Полный цикл «изменил код → увидел лог»:
```
# В контейнере:
cd /workspace/build && ninja yt-server-http_proxy ytserver-all
# На хосте:
docker cp ytsaurus-dev:/workspace/build/yt/yt/server/all/ytserver-all /tmp/ytserver-all
# Стопим старый кластер (Ctrl+C если на --sync), потом:
yt_local start --enable-debug-logging --proxy-port 8000 --fqdn localhost --ytserver-all-path /tmp/ytserver-all --sync
# Триггерим:
curl -s 'http://localhost:8000/api/v4/get?path=//sys/@cluster_name'
# Смотрим:
grep 'Running driver request' /tmp/yt_local/*/logs/http-proxy-0.debug.log | tail
```

## 12. yt_local stop падает с `FileNotFoundError: ... stderrs/stderr.watcher`

Безобидный bug в yt_local: при остановке watcher'а пытается прочитать его stderr-файл, который не успел создаться (быстрый exit). Кластер на самом деле остановлен корректно.

Хочется обойти — добавь `--delete` к `yt_local stop` или просто `rm -rf /tmp/yt_local/*`.

## 13. `yt_local: error: unrecognized arguments: Size User Date Modified Name`

Не команда yt_local виновата, а то, что у тебя alias `ls` показывает таблицу с заголовком. Когда сделал `yt_local stop $(ls /tmp/yt_local | head -1)` — шелл подставил первую строчку из табличного вывода, а это заголовок.

Лечится:
```
yt_local stop $(command ls /tmp/yt_local | head -1)
```
(`command ls` обходит alias и даёт сырой вывод.)

## 14. `Command-line option --enable-debug-logging is deprecated and will be eventually removed`

Это предупреждение, не ошибка — пока флаг работает. Если будет убран, использовать:
```
yt_local start --log-level debug ...
```

## 15. Опечатка `import linked yt.wraper - ok` в install_locally.sh

Очевидная опечатка апстрима (wraper → wrapper) в `yt/python/packages/ytsaurus-client-trunk-dev/install_locally.sh`. Это просто echo, на работоспособность не влияет. Не паникуй когда увидишь.
