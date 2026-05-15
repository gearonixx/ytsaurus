# Security observations

Заметки про потенциальные проблемы безопасности, замеченные по ходу настройки локального стенда. **Не полноценный аудит** — только то, на что глаз упал в процессе работы.

| Файл | Про что |
|---|---|
| [`our-debug-logging.md`](our-debug-logging.md) | Анализ нашего `YT_LOG_DEBUG` в `http_proxy/context.cpp` |
| [`our-environment.md`](our-environment.md) | Container privileges, UI auth, pip editable, SSH key, claude bypass |
| [`codebase-observations.md`](codebase-observations.md) | Что заметили в самом репо YTsaurus (`AbortOnOom`, cgroup v2) |
