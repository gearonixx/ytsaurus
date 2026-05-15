# Анализ `YT_LOG_DEBUG` в `http_proxy/context.cpp`

```cpp
YT_LOG_DEBUG(
    "Running driver request "
    "(Id: %v, CommandName: %v, AuthenticatedUser: %v, UserTag: %v, "
    "UserRemoteAddress: %v, UserTokenPresent: %v, UserTokenLength: %v, "
    "ServiceTicketPresent: %v, ServiceTicketLength: %v, LoggingTags: %v, "
    "Parameters: %v, InputStreamPresent: %v, OutputStreamPresent: %v, "
    "ResponseParametersConsumerPresent: %v, ResponseParametersFinishedCallbackPresent: %v, "
    "MemoryUsageTrackerPresent: %v)",
    ...
    Descriptor_
        ? ConvertToYsonString(
            HideSecretParameters(Descriptor_->CommandName, driverRequest.Parameters),
            EYsonFormat::Text).ToString()
        : TString("<no-descriptor>"),
    ...
);
```

## Что сделано правильно

- **Не печатается сам `UserToken`**, только `UserTokenPresent` и `UserTokenLength`.
- **Не печатается `ServiceTicket`**, только наличие и длина.
- **`Parameters` идут через `HideSecretParameters(...)`** — для известных команд секретные параметры маскируются.

## Что требует осторожности

### 1. `HideSecretParameters` работает только при наличии `Descriptor_`

В коде явно: `Descriptor_ ? HideSecretParameters(...) : "<no-descriptor>"`. То есть для запросов без descriptor'а параметры выливаются **сырыми**. Это потенциальный канал для info-disclosure: если придёт неизвестная команда (или custom-команда от прокси-плагина), её параметры попадут в лог без маскировки.

**Что проверить:**
- Когда `Descriptor_` может быть `nullptr`? Найти все пути, в которых это возможно.
- Залогировать факт `<no-descriptor>` отдельно с категорией WARNING, чтобы не пропустить такой случай в проде.

### 2. `HideSecretParameters` маскирует **известные** параметры известных команд

Это white-list-подход внутри `HideSecretParameters`. Если в новую команду добавят поле с токеном/ключом — оно по умолчанию **не маскируется**, пока кто-то явно не добавит его в список.

**Что проверить:**
- Найди реализацию `HideSecretParameters` и список замаскированных полей.
- Покрытие тестами: `HideSecretParameters_test*`. Убедиться, что добавление нового секретного параметра падает (red test) пока не обновишь маску.

### 3. `UserRemoteAddress` — PII

Адрес клиента (`tcp://[::1]:60036` в твоём примере) — это **персональные данные** по GDPR/152-ФЗ. Для local-dev — фигня. Для prod с retention 30 дней — compliance-вопрос:
- срок хранения должен соответствовать политикам компании
- лог-pipe не должен утекать в third-party аналитику без анонимизации
- pseudonymization/hashing адреса — стандартная практика

### 4. `LoggingTags` пишутся сырьём

В коде: `driverRequest.LoggingTags` подставляется напрямую через `%v`. **Нет** прохождения через какую-либо маску. Если в LoggingTags попадает что-то чувствительное (например, idempotency tokens, internal request signatures) — оно выливается в debug-лог.

**Что проверить:**
- Кто и что кладёт в `LoggingTags`? Грепни `SetLoggingTags`/`logging_tags` по репо.
- Если там потенциально появляются ключи — нужна аналогичная маска.

### 5. Timing-side-channel через `UserTokenLength` / `ServiceTicketLength`

Печать длины токена — намного слабее, чем сам токен, но даёт инфу. Если у тебя в системе токены ровно двух форматов (например, OAuth=N байт, TVM-ticket=M байт), длина по сути выдаёт тип. В YTsaurus длины обычно из одной популяции (UUID-подобные), так что риск минимальный, но не нулевой.

## Что бы я добавил

- Маркировку лога: `YT_LOG_DEBUG` пишет только в `.debug.log`, который **не должен включаться в production**. В скриптах `local_setup/05-start-cluster.sh` это явно для dev. Но если кто-то скопирует команду в prod-конфиг — поток PII пойдёт в файлы.
- Pre-commit hook / CI-чек на наличие сырого `driverRequest.Parameters` в формат-строках без `HideSecretParameters` (или `<<` на `TErrorAttribute("user_token", ...)`-подобные).
