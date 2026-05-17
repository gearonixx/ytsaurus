

● Да, в CLI это флаг --control-attributes — он принимает YSON-словарь, и оттуда поля и заезжают в ControlAttributes. Например:

  **yt read-table --control-attributes '{enable_row_index=%true; enable_range_index=%true}' //tmp/foo**

---


 Цепочка такая, по шагам:

  1. CLI: yt read-table --control-attributes '{enable_row_index=%true}' //tmp/foo. Это Python-обёртка (yt CLI), она просто кладёт YSON-строку в параметры HTTP-запроса к http_proxy.
  2. http_proxy принимает HTTP-запрос, парсит query string и тело, формирует YSON-словарь параметров команды read_table и отдаёт это в driver.
  3. Driver (общий слой команд, yt/yt/client/driver) видит команду read_table и десериализует параметры в C++-структуру — там есть подструктура control_attributes, которая через REGISTER_YSON_STRUCT маппится в
  TControlAttributesConfig. YSON-ключ enable_row_index автоматически превращается в поле EnableRowIndex (snake_case ↔ CamelCase).
  4. Native client: driver вызывает IClient::CreateTableReader(...), передавая TTableReaderOptions. Дальше, в коде, который ты раньше показывал, поля из ControlAttributes перекладываются в Options:

  Options.EnableRowIndex = ControlAttributes->EnableRowIndex;

  5. Schemaless reader: ниже эти Options.EnableRowIndex/TableIndex/... едут в NTableClient::TTableReaderOptions (через ToInternalTableReaderOptions) и оттуда — в schemaless chunk reader, который при чтении каждого ряда
  добавляет к нему системные «псевдоколонки» ($row_index, $range_index и т.д.).
  6. Форматтер вывода (yson/json/dsv) на выходе видит эти атрибуты строки и пишет их как <row_index=42> (в YSON это attribute-обёртка над строкой) или как отдельные ключи в JSON.

  Итог: одна YSON-строчка в CLI протекает через proxy → driver → native client → chunk reader → формат вывода, и в результате каждая строка таблицы в выводе помечается номером строки/таблицы/range'а.

✻ Worked for 19s

---

По дефолту Python CLI yt ходит именно через http_proxy (HTTP REST), но это настраивается. Бывает три варианта бэкенда:

  - HTTP proxy — дефолт CLI. Питон сериализует команду в HTTP-запрос (POST /api/v4/read_table, параметры в заголовках/query, тело — данные), proxy парсит и зовёт driver.
  - RPC proxy — если в конфиге backend=rpc или ставится через yt_driver_rpc_bindings. Тогда CLI говорит по нативному YT-RPC напрямую с rpc-proxy, без HTTP. Быстрее и держит стримы лучше.
  - Native — driver прямо в процессе CLI (через C++-биндинги), без прокси вообще. Редко, обычно для тестов.

  Проверить можно так: yt --proxy <cluster> get //sys/@type — если в ~/.yt/config или env YT_CONFIG_PATCHES есть backend=rpc, то RPC. По умолчанию у yt CLI это HTTP.


---

# TRANSACTIONS



● Потому что YT-транзакция — это не локальная вещь в БД, а объект на мастере (со своим ID, таймаутом, родителем), и клиенту нужен «хэндл» этого объекта в процессе, чтобы:

  1. Делать API-вызовы внутри транзакции. Смотри: ITransaction : IClientBase — у транзакции те же методы, что и у клиента (CreateTableReader, GetNode, WriteTable, ...), но они автоматически подставляют transaction_id в
  каждый RPC. Без этого пришлось бы вручную таскать ID в каждый вызов.
  2. Слать пинги: транзакция на мастере жива, пока клиент её пингует. Клиентский объект в фоне дергает PingTransaction каждые N секунд, и если процесс упал — мастер сам её аборнет по таймауту.
  3. Управлять жизненным циклом — Commit, Abort, Detach, Flush, плюс хук Committed для коллбэков «после успешного коммита».
  4. Держать локальное состояние: буферы записи в динамические таблицы (IDynamicTableTransaction), список «чужих» транзакций (RegisterAlienTransaction — для согласованной коммита по нескольким кластерам), и т.п.

  Грубо: на мастере транзакция = запись «id=X, owner=Y, parent=Z, expires_at=...», а ITransaction в клиенте = пинг-демон + обёртка над IClient, которая ко всем вызовам клеит transaction_id=X.

✻ Worked for 16s
  
❯ еще проще                                                                                                                                                                                                                     

● Транзакция в YT живёт на мастере как объект с id. ITransaction в клиенте — это просто обёртка над IClient, которая ко всем твоим вызовам автоматически подставляет этот id, плюс пингует мастер чтобы транзакция не протухла.
  Без неё пришлось бы в каждый запрос вручную писать transaction_id=....

---


● Потому что класс многопоточный: callback'и из TPeriodicExecutor (фоновый поток, который пингует/проверяет транзакции) дергают OnTransactionAborted и пишут в AbortedTransactionIds_, TransactionIdToCustomError_,
  TransactionIdToProbeExecutor_; а с других потоков владелец параллельно зовёт StartListen/StopListen/Probe/ValidateAborted, которые читают/пишут те же поля. std::vector и THashMap сами не потокобезопасные, поэтому нужен
  lock.

---
---


● Потому что yt read-table всегда сам создаёт временную транзакцию, даже если ты её не просил. Это нужно для консистентности чтения: пока ты тянешь чанки, никто не должен их удалить или переписать таблицу под тобой.

  Что происходит за кадром:

  1. CLI зовёт read_table. Driver внутри стартует короткоживущую транзакцию (видишь её id 1-9ce-10001-bb04 в логах).
  2. На таблицу ставится snapshot lock в рамках этой транзакции — мастер фиксирует «эта таблица в момент T выглядела вот так, её чанки нельзя GC-нуть, пока tx жива».
  3. Reader получает список chunk_id, идёт за байтами на data nodes.
  4. Транзакцию детачат (State: Detached в логе) — клиент перестаёт её пинговать, но она ещё живёт по таймауту на мастере, чтобы чанки точно не убрали, пока чтение не закончится.
  5. По окончании чтения tx истекает/аборнется, и снапшот-лок снимается.

  Поэтому в логах ты видишь и Started listening for transaction, и пинги CypressTransactionService.PingTransaction, и потом Transaction is not in pingable state (State: Detached) — это всё цикл «защитной» транзакции вокруг
  твоего одного read-table.

  Если бы ты сам передал --tx <id>, driver не создавал бы новую, а пользовался твоей.

✻ Worked for 18s
  