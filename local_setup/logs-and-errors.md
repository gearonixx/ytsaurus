# Как смотреть логи и ошибки YTsaurus — не только через `*.log`

Файлы — это **низший** слой. Над ним есть несколько более удобных способов получить ту же информацию. Конкретно для типичного флоу «я добавил `THROW_ERROR_EXCEPTION` с `TErrorAttribute(...)` и хочу увидеть атрибуты» работают **четыре** канала, и три из них не требуют читать файлы вообще.

## 0. Текстовые `*.log` / `*.debug.log`

Что мы уже знаем. INFO+ в обычных, DEBUG+ в `.debug.log` (нужен `--enable-debug-logging`). Каждая строка — текст с экранированными `\n`. Грепать неудобно, особенно для error-атрибутов — они сваливаются в плоский текст.

```
tail -f /tmp/yt_local/*/logs/http-proxy-0.debug.log
grep '\s[WE]\s' /tmp/yt_local/*/logs/*.log
```

## 1. Структурированные JSON-логи (`--enable-structured-logging`)

Самый недооценённый канал. Включается флагом у `yt_local start`:
```
yt_local start --enable-structured-logging ...
```

Появляются дополнительные файлы рядом с обычными:
```
/tmp/yt_local/<id>/logs/http-proxy-0.json.log
/tmp/yt_local/<id>/logs/master-0-0.json.log
...
```

Каждая строка — JSON-объект:
```json
{
  "timestamp": "2026-05-15T10:23:45.123Z",
  "level": "E",
  "category": "HttpProxy",
  "thread": "Worker",
  "trace_id": "abcd-...",
  "request_id": "ffff-...",
  "message": "User memory limit exceeded",
  "attributes": {
    "heavy_request_memory_limit": 1073741824,
    "heavy_request_memory_usage": 2147483648,
    "user": "root"
  }
}
```

Все твои `<< TErrorAttribute("name", value)` приедут как поля внутри `attributes`. Удобно жевать `jq`:

```bash
# Только ошибки с их атрибутами
jq 'select(.level=="E") | {ts:.timestamp, msg:.message, attrs:.attributes}' \
   /tmp/yt_local/*/logs/http-proxy-0.json.log

# Все ошибки HeavyRequest-tracker'а
jq 'select(.attributes.heavy_request_memory_limit != null)' \
   /tmp/yt_local/*/logs/http-proxy-0.json.log

# Live-стрим: только ERROR от http_proxy
tail -F /tmp/yt_local/*/logs/http-proxy-0.json.log | jq -c 'select(.level=="E")'
```

## 2. HTTP-ответ клиенту (`X-YT-Error`)

`THROW_ERROR_EXCEPTION` внутри обработчика http_proxy **уже** приезжает клиенту в ответе. Все `TErrorAttribute` попадают в JSON-структуру в заголовке `X-YT-Error`:

```bash
$ curl -sv 'http://localhost:8000/api/v4/read_table?path=//tmp/huge_table' 2>&1 \
    | grep -E 'X-YT-(Response-Code|Error)'

< X-YT-Response-Code: 1
< X-YT-Error: {"code":1,"message":"User memory limit exceeded","attributes":{"heavy_request_memory_limit":1073741824,"heavy_request_memory_usage":2147483648}}
```

Через CLI ещё проще — он сам распарсит:
```bash
$ yt --proxy localhost:8000 read-table //tmp/huge_table
        Error: User memory limit exceeded
            heavy_request_memory_limit: 1073741824
            heavy_request_memory_usage: 2147483648
            origin: localhost on 2026-05-15T...
```

В web-UI: открой DevTools → Network, тыкни на упавший запрос → Response Headers → `X-YT-Error`.

**Для отладки клиентских ошибок это нормальный путь — никакие файлы открывать не нужно.**

## 3. Orchid — live-интроспекция

Каждый компонент YT держит endpoint `/orchid` с деревом своего состояния, доступным по HTTP:

```bash
# Что вообще есть у http_proxy
yt --proxy localhost:8000 list //sys/http_proxies/localhost:8000/orchid

# Memory tracker в реальном времени (без ожидания триггера ошибки)
yt --proxy localhost:8000 get //sys/http_proxies/localhost:8000/orchid/memory_usage

# Профилирование
yt --proxy localhost:8000 get //sys/http_proxies/localhost:8000/orchid/profiling/yt/memory/usage
```

Или прямо `curl`:
```bash
curl -s 'http://localhost:8000/api/v4/get?path=//sys/http_proxies/localhost:8000/orchid/memory_usage' | jq
```

Преимущество: видишь **живые** counter'ы и состояние, не дожидаясь, пока что-то упадёт.

## 4. Prometheus-метрики

Если запустить с `run_local_cluster.sh` — Prometheus поднимется автоматически и будет скрэпить `:9090`. У `yt_local` руками: каждый компонент отдаёт метрики через тот же `/orchid`, и есть endpoint в формате Prometheus:

```bash
curl -s 'http://localhost:8000/solomon/all'   # или /metrics — зависит от билда
```

Для трекинга `heavy_request_memory_*` будут метрики типа:
```
yt_memory_usage_used{category="heavy_request",pool_tag="..."} 2147483648
yt_memory_usage_limit{category="heavy_request",pool_tag="..."} 1073741824
```

Дальше — Grafana, алерты, графики.

## 5. Trace ID для корреляции между компонентами

В каждой строке текстового лога в конце есть колонка `Control fffee6b7c3...` — это trace_id (или request_id). Один HTTP-запрос проходит через `http_proxy → master → node` (или scheduler) — и в каждом компоненте те же trace_id'ы. Чтобы собрать всю цепочку:

```bash
TID="fffee6b7c3..."
grep -rh "$TID" /tmp/yt_local/*/logs/*.debug.log | sort
```

Или, в структурированных логах:
```bash
for f in /tmp/yt_local/*/logs/*.json.log; do
    jq -c "select(.trace_id==\"$TID\")" "$f"
done | sort
```

Получишь хронологию одного запроса по всем компонентам сразу.

## 6. Когда что использовать

| Сценарий | Где смотреть |
|---|---|
| «Мой curl/CLI-клиент получил странную ошибку — что не так» | **`X-YT-Error`** в ответе (см. п.2) |
| «Хочу алерт на ERROR'ы с конкретным атрибутом» | **JSON-логи + jq** (п.1) |
| «Сколько прямо сейчас памяти жрёт heavy_request у root'а» | **Orchid** (п.3) |
| «Запрос завис, не знаю где, надо весь его trace» | **Trace ID grep** (п.5) — лучше по JSON-логам |
| «Графики и история по часам/дням» | **Prometheus + Grafana** (п.4) |
| «Странность в проде, копаюсь в 100MB истории» | **Текстовые `.debug.log`** — последний резорт |

## Полезные one-liner'ы

```bash
# Все ERROR'ы в любом компоненте за последние 100 строк каждого:
for f in /tmp/yt_local/*/logs/*.json.log; do
    echo "=== $f ==="
    tail -100 "$f" | jq -c 'select(.level=="E")'
done

# Профиль: топ-10 категорий по количеству ERROR'ов
cat /tmp/yt_local/*/logs/*.json.log \
    | jq -r 'select(.level=="E") | .category' \
    | sort | uniq -c | sort -rn | head

# Все TErrorAttribute'ы из ошибок http_proxy
jq 'select(.level=="E") | .attributes | keys' \
   /tmp/yt_local/*/logs/http-proxy-0.json.log | sort -u
```
