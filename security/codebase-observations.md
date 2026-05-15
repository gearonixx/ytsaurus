# Наблюдения в самом репо YTsaurus

Не аудитил весь YTsaurus, но по дороге заметил:

## 1. `AbortOnOom` в `library/cpp/yt/memory/new-inl.h`

`AbortOnOom` зовётся когда `malloc()` вернул `nullptr`. Не уязвимость, но это **fail-stop**: один OOM в любой части → кластер падает.

Для DoS — годится: если злоумышленник нагрузит кластер запросами, упирающими в `heavy_request_memory_limit`, можно вызвать каскадный crash.

Защита — `MemoryUsageTracker`, но он работает, только если корректно проставлен на каждой аллокации. Если есть путь, где `malloc()` вызывается **без** проверки tracker'а (например, через внешние библиотеки, тяжёлые stl-контейнеры, ситуативные `make_shared`) — этот путь обходит защиту и приходит сразу к `AbortOnOom`.

## 2. `TResourceTracker::GetAnonymousMemoryLimit` читает cgroup v1

В `yt/yt/library/profiling/resource_tracker/resource_tracker.cpp` код ищет controller `"memory"` — это **cgroup v1** API. В **cgroup v2** контроллеры унифицированы, такого имени нет → код не находит memory cgroup → `AnonymousMemoryLimit_` остаётся 0 → `ProposeHeapMemoryLimit(0, ...)` возвращает пустой лимит → `SetMemoryLimit` не вызывается → **hard heap-лимит TCMalloc не выставляется вообще**.

Это **не** security-bug сам по себе, но **деградация защитного механизма** в современных окружениях (cgroup v2 — дефолт в systemd 244+, k8s 1.25+, новых дистрибутивах вроде Arch). Последствия:
- Меньше шансов, что TCMalloc остановится на разумном пороге → больше шансов получить OS-уровневый OOM-kill.
- Менее предсказуемое поведение под нагрузкой.

Чинится добавлением чтения cgroup v2 (`memory.max`, `memory.current` из `/sys/fs/cgroup/<path>/`).
