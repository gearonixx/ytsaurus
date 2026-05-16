#include "table_reader.h"

#include "client.h"

#include <yt/yt/client/api/private.h>

#include <yt/yt/ytlib/table_client/table_read_spec.h>

#include <yt/yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_host.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_options.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/yt/ytlib/chunk_client/data_source.h>
#include <yt/yt/ytlib/chunk_client/dispatcher.h>
#include <yt/yt/ytlib/chunk_client/helpers.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/ytlib/table_client/config.h>
#include <yt/yt/ytlib/table_client/schemaless_multi_chunk_reader.h>

#include <yt/yt/ytlib/transaction_client/transaction_listener.h>

#include <yt/yt/ytlib/object_client/helpers.h>

#include <yt/yt/client/api/table_reader.h>
#include <yt/yt/client/api/transaction.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/client/chunk_client/chunk_replica.h>

#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/row_batch.h>

#include <yt/yt/client/ypath/rich.h>

#include <yt/yt/core/concurrency/scheduler.h>
#include <yt/yt/core/concurrency/throughput_throttler.h>

#include <yt/yt/core/misc/protobuf_helpers.h>

#include <yt/yt/core/rpc/public.h>

#include <library/cpp/yt/memory/range.h>

namespace NYT::NApi::NNative {

using namespace NChunkClient;
using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NYPath;

////////////////////////////////////////////////////////////////////////////////

class TTableReader
    : public ITableReader
    , public TTransactionListener
{
public:
    TTableReader(
        TTableReaderConfigPtr config,
        TTableReaderOptions options,
        IClientPtr client,
        NApi::ITransactionPtr transaction,
        TRichYPath richPath,
        TNameTablePtr nameTable,
        TColumnFilter columnFilter,
        IThroughputThrottlerPtr bandwidthThrottler,
        IThroughputThrottlerPtr rpsThrottler,
        IMemoryUsageTrackerPtr memoryUsageTracker)
        : Config_(std::move(config))
        , Options_(std::move(options))
        , Client_(std::move(client))
        , Transaction_(std::move(transaction))
        , RichPath_(std::move(richPath))
        , NameTable_(std::move(nameTable))
        , ColumnFilter_(std::move(columnFilter))
        , BandwidthThrottler_(std::move(bandwidthThrottler))
        , RpsThrottler_(std::move(rpsThrottler))
        , TransactionId_(Transaction_ ? Transaction_->GetId() : NullTransactionId)
        , MemoryUsageTracker_(std::move(memoryUsageTracker))
    {
        YT_VERIFY(Config_);
        YT_VERIFY(Client_);

        ReadyEvent_ = BIND(&TTableReader::DoOpen, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
            .Run();
    }

    IUnversionedRowBatchPtr Read(const TRowBatchReadOptions& options) override
    {
        if (NProfiling::GetCpuInstant() > ReadDeadline_) {
            THROW_ERROR_EXCEPTION(NTableClient::EErrorCode::ReaderDeadlineExpired, "Reader deadline expired");
        }

        if (IsAborted() || !ReadyEvent_.IsSet() || !ReadyEvent_.GetOrCrash().IsOK()) {
            return CreateEmptyUnversionedRowBatch();
        }

        YT_VERIFY(Reader_);
        return Reader_->Read(options);
    }

    TFuture<void> GetReadyEvent() const override
    {
        if (!ReadyEvent_.IsSet() || !ReadyEvent_.GetOrCrash().IsOK()) {
            return ReadyEvent_;
        }

        if (IsAborted()) {
            return MakeFuture(GetAbortError());
        }

        YT_VERIFY(Reader_);
        return Reader_->GetReadyEvent();
    }

    i64 GetStartRowIndex() const override
    {
        YT_VERIFY(Reader_);
        return StartRowIndex_;
    }

    i64 GetTotalRowCount() const override
    {
        YT_VERIFY(Reader_);
        return Reader_->GetTotalRowCount();
    }

    NChunkClient::NProto::TDataStatistics GetDataStatistics() const override
    {
        YT_VERIFY(Reader_);
        return Reader_->GetDataStatistics();
    }

    const TNameTablePtr& GetNameTable() const override
    {
        YT_VERIFY(Reader_);
        return Reader_->GetNameTable();
    }

    const TTableSchemaPtr& GetTableSchema() const override
    {
        YT_VERIFY(Reader_);
        return TableSchema_;
    }

    const std::vector<std::string>& GetOmittedInaccessibleColumns() const override
    {
        YT_VERIFY(Reader_);
        return OmittedInaccessibleColumns_;
    }

private:
    const TTableReaderConfigPtr Config_;
    TTableReaderOptions Options_;
    const IClientPtr Client_;
    const NApi::ITransactionPtr Transaction_;
    const TRichYPath RichPath_;
    const TNameTablePtr NameTable_;
    const TColumnFilter ColumnFilter_;
    const IThroughputThrottlerPtr BandwidthThrottler_;
    const IThroughputThrottlerPtr RpsThrottler_;
    const TTransactionId TransactionId_;
    const IMemoryUsageTrackerPtr MemoryUsageTracker_;

    TFuture<void> ReadyEvent_;
    ISchemalessMultiChunkReaderPtr Reader_;
    TTableSchemaPtr TableSchema_;
    std::vector<std::string> OmittedInaccessibleColumns_;
    i64 StartRowIndex_;
    NProfiling::TCpuInstant ReadDeadline_ = Max<NProfiling::TCpuInstant>();

    // @gearonixx Асинхронная "тяжёлая" часть открытия читателя таблицы.
    // Запускается из конструктора через BIND(...).AsyncVia(ReaderInvoker).Run(),
    // а её результат сохраняется в ReadyEvent_. Пока эта корутина не завершилась
    // успешно, Read() будет возвращать пустой батч, а GetReadyEvent() — отдавать
    // именно этот future. Делается всё в фоне, чтобы CreateTableReader() сразу
    // вернул объект, не блокируя вызывающий поток на походах в мастер.
    void DoOpen()
    {
        // @gearonixx Локальный Logger нужен макросу YT_LOG_DEBUG: он ищет
        // переменную с именем Logger в текущей области видимости.
        const auto& Logger = ApiLogger();
        YT_LOG_DEBUG("@@gearonixx_driver TTableReader::DoOpen started (Path: %v, TransactionId: %v)",
            RichPath_,
            TransactionId_);

        // @gearonixx Полный дамп Options_ (NApi::TTableReaderOptions) — собственных
        // полей + унаследованных из TTransactionalOptions и TSuppressableAccessTrackingOptions.
        YT_LOG_DEBUG("gearonixx_driver_2 TTableReader::Options_ dump "
            "(Unordered: %v, OmitInaccessibleColumns: %v, OmitInaccessibleRows: %v, "
            "EnableTableIndex: %v, EnableRowIndex: %v, EnableRangeIndex: %v, "
            "EnableTabletIndex: %v, EnableAnyUnpacking: %v, HasConfig: %v, "
            "TransactionId: %v, Ping: %v, PingAncestors: %v, "
            "SuppressTransactionCoordinatorSync: %v, SuppressUpstreamSync: %v, "
            "SuppressStronglyOrderedTransactionBarrier: %v, "
            "SuppressAccessTracking: %v, SuppressModificationTracking: %v, "
            "SuppressExpirationTimeoutRenewal: %v)",
            Options_.Unordered,
            Options_.OmitInaccessibleColumns,
            Options_.OmitInaccessibleRows,
            Options_.EnableTableIndex,
            Options_.EnableRowIndex,
            Options_.EnableRangeIndex,
            Options_.EnableTabletIndex,
            Options_.EnableAnyUnpacking,
            static_cast<bool>(Options_.Config),
            Options_.TransactionId,
            Options_.Ping,
            Options_.PingAncestors,
            Options_.SuppressTransactionCoordinatorSync,
            Options_.SuppressUpstreamSync,
            Options_.SuppressStronglyOrderedTransactionBarrier,
            Options_.SuppressAccessTracking,
            Options_.SuppressModificationTracking,
            Options_.SuppressExpirationTimeoutRenewal);

        // Transform NApi::TTableReaderOptions into NTableClient::TTableReader{Options,Config}.
        // @gearonixx Если пользователь не передал свой TTableReaderConfig — создаём
        // конфиг по умолчанию (New<T> — это аналог std::make_shared для YT-объектов
        // с интрузивным рефкаунтом, TIntrusivePtr).
        auto tableReaderConfig = Options_.Config ? Options_.Config : New<TTableReaderConfig>();
        // @gearonixx Конвертируем "клиентские" опции (NApi уровня) во "внутренние"
        // опции table_client — там живут флаги вроде EnableRowIndex / EnableRangeIndex,
        // которые нужны самим chunk-ридерам, а не публичному API.
        auto tableReaderOptions = ToInternalTableReaderOptions(Options_);
        YT_LOG_DEBUG("@@gearonixx_driver Reader config and internal options prepared");

        // @gearonixx Уникальный id всей сессии чтения. Прокидывается во все
        // подзапросы (к мастеру, к нодам), чтобы по логам можно было собрать
        // одну цепочку обращений, относящихся к этому открытию таблицы.
        auto readSessionId = TReadSessionId::Create();
        YT_LOG_DEBUG("@@gearonixx_driver Read session id created (ReadSessionId: %v)", readSessionId);

        // @gearonixx Собираем "запрос к мастеру": какой путь читаем, в какой
        // транзакции, какие атрибуты узла нам нужны, и параметры доступности
        // чанков. Сам поход в мастер произойдёт ниже, в FetchSingleTableReadSpec.
        auto fetchTableReadSpecOptions = TFetchSingleTableReadSpecOptions{
            .RichPath = RichPath_,
            .Client = Client_,
            .TransactionId = Options_.TransactionId,
            .ReadSessionId = readSessionId,
            .GetUserObjectBasicAttributesOptions = TGetUserObjectBasicAttributesOptions{
                // It's fine to ignore SuppressModificationTracking, since read requests can't modify table.
                // @gearonixx SuppressAccessTracking — не апдейтить access_time таблицы;
                // SuppressExpirationTimeoutRenewal — не продлевать TTL у tmp-таблиц.
                // OmitInaccessibleColumns/Rows — тихо выбрасывать те колонки/строки,
                // к которым у пользователя нет доступа, вместо ошибки доступа.
                .SuppressAccessTracking = tableReaderConfig->SuppressAccessTracking || Options_.SuppressAccessTracking,
                .SuppressExpirationTimeoutRenewal = tableReaderConfig->SuppressExpirationTimeoutRenewal || Options_.SuppressExpirationTimeoutRenewal,
                .OmitInaccessibleColumns = Options_.OmitInaccessibleColumns,
                .OmitInaccessibleRows = Options_.OmitInaccessibleRows,
            },
            .FetchChunkSpecConfig = Config_,
            // @gearonixx FetchParityReplicas — тянуть ли parity-части erasure-чанков
            // (нужно, если включён auto-repair и какие-то реплики недоступны).
            .FetchParityReplicas = tableReaderConfig->EnableAutoRepair,
            // @gearonixx Что делать, если часть чанков недоступна: упасть с ошибкой,
            // пропустить их или пытаться чинить (политики задаются конфигом).
            .UnavailableChunkStrategy = tableReaderConfig->UnavailableChunkStrategy,
            .ChunkAvailabilityPolicy = tableReaderConfig->ChunkAvailabilityPolicy,
        };

        // @gearonixx Опции собственно чтения чанков с нод (не путать с метаданными
        // выше): сюда кладётся трекер памяти, WorkloadDescriptor (приоритет/категория
        // трафика — system/user_batch/realtime и т. п.) и тот же ReadSessionId.
        TClientChunkReadOptions chunkReadOptions;
        chunkReadOptions.MemoryUsageTracker = MemoryUsageTracker_;
        chunkReadOptions.WorkloadDescriptor = tableReaderConfig->WorkloadDescriptor;
        // @gearonixx Аннотации workload — это просто строки, которые попадают в
        // логи на нодах. Добавляем путь таблицы, чтобы по логу ноды было видно,
        // ради чтения какой таблицы пришёл запрос.
        chunkReadOptions.WorkloadDescriptor.Annotations.push_back(Format("TablePath: %v", RichPath_.GetPath()));
        chunkReadOptions.ReadSessionId = readSessionId;
        YT_LOG_DEBUG("@@gearonixx_driver Chunk read options prepared, fetching table read spec from master");
        // @gearonixx Синхронный (с точки зрения этой корутины) поход в мастер:
        // резолвит путь, читает атрибуты, забирает список чанков и формирует
        // "спецификацию чтения" — DataSourceDirectory + ChunkSpecs.
        auto tableReadSpec = FetchSingleTableReadSpec(fetchTableReadSpecOptions);

        // @gearonixx Лог всех полей TTableReadSpec, что вернула FetchSingleTableReadSpec.
        // Сама структура содержит два поля: DataSourceDirectory (справочник источников)
        // и DataSliceDescriptors (список слайсов с чанками). Разворачиваем оба.
        {
            const auto& dataSources = tableReadSpec.DataSourceDirectory->DataSources();
            i64 totalChunkCount = 0;
            for (const auto& slice : tableReadSpec.DataSliceDescriptors) {
                totalChunkCount += std::ssize(slice.ChunkSpecs);
            }
            YT_LOG_DEBUG("gearonixx_driver_2 TTableReadSpec summary "
                "(DataSourceCount: %v, DataSliceCount: %v, TotalChunkCount: %v)",
                dataSources.size(),
                tableReadSpec.DataSliceDescriptors.size(),
                totalChunkCount);

            // @gearonixx Для каждого источника печатаем все его поля
            // (см. TDataSource в yt/yt/ytlib/chunk_client/data_source.h).
            for (int i = 0; i < std::ssize(dataSources); ++i) {
                const auto& ds = dataSources[i];
                YT_LOG_DEBUG("gearonixx_driver_2 TTableReadSpec.DataSource[%v] "
                    "(Type: %v, Path: %v, ObjectId: %v, Foreign: %v, "
                    "SchemaColumnCount: %v, Columns: %v, OmittedInaccessibleColumns: %v, "
                    "Timestamp: %v, RetentionTimestamp: %v, ColumnRenameDescriptorCount: %v, "
                    "VirtualKeyPrefixLength: %v, HasVirtualValueDirectory: %v, "
                    "Account: %v, ClusterName: %v, HasRlsReadSpec: %v, HasInputQuerySpec: %v)",
                    i,
                    ds->GetType(),
                    ds->GetPath(),
                    ds->GetObjectId(),
                    ds->GetForeign(),
                    ds->Schema() ? ds->Schema()->GetColumnCount() : 0,
                    ds->Columns(),
                    ds->OmittedInaccessibleColumns(),
                    ds->GetTimestamp(),
                    ds->GetRetentionTimestamp(),
                    ds->ColumnRenameDescriptors().size(),
                    ds->GetVirtualKeyPrefixLength(),
                    static_cast<bool>(ds->GetVirtualValueDirectory()),
                    ds->GetAccount(),
                    ds->GetClusterName(),
                    ds->GetRlsReadSpec().has_value(),
                    ds->GetInputQuerySpec().has_value());
            }

            // @gearonixx Для каждого слайса — индекс источника, индекс ренжа,
            // сколько в нём чанков и опциональный VirtualRowIndex.
            for (int i = 0; i < std::ssize(tableReadSpec.DataSliceDescriptors); ++i) {
                const auto& slice = tableReadSpec.DataSliceDescriptors[i];
                YT_LOG_DEBUG("gearonixx_driver_2 TTableReadSpec.DataSlice[%v] "
                    "(DataSourceIndex: %v, RangeIndex: %v, ChunkCount: %v, VirtualRowIndex: %v)",
                    i,
                    slice.GetDataSourceIndex(),
                    slice.GetRangeIndex(),
                    slice.ChunkSpecs.size(),
                    slice.VirtualRowIndex);
            }
        }

        // @gearonixx Здесь читается ровно одна таблица, поэтому источник данных
        // в директории должен быть ровно один — иначе это баг выше по стеку.
        YT_VERIFY(tableReadSpec.DataSourceDirectory->DataSources().size() == 1);
        const auto& dataSource = tableReadSpec.DataSourceDirectory->DataSources().front();
        // @gearonixx Сохраняем схему таблицы и список колонок, которые мастер
        // выкинул из-за отсутствия прав — пользователь сможет достать их через
        // GetTableSchema() / GetOmittedInaccessibleColumns().
        TableSchema_ = dataSource->Schema();
        OmittedInaccessibleColumns_ = dataSource->OmittedInaccessibleColumns();
        YT_LOG_DEBUG("@@gearonixx_driver Table read spec fetched (SchemaColumnCount: %v, OmittedInaccessibleColumnCount: %v)",
            TableSchema_ ? TableSchema_->GetColumnCount() : 0,
            OmittedInaccessibleColumns_.size());

        YT_LOG_DEBUG("@@gearonixx_driver Creating schemaless multi-chunk reader (Unordered: %v)", Options_.Unordered);
        // @gearonixx Создаём настоящий читатель, который умеет ходить по всем
        // чанкам таблицы. "Schemaless" — потому что отдаёт TUnversionedRow без
        // привязки к конкретной схеме на уровне типов C++; "MultiChunk" — потому
        // что прячет за собой целую пачку chunk-ридеров. "Appropriate" выбирает
        // ordered/unordered вариант в зависимости от Options_.Unordered.
        // der просто собирает объект ридера в памяти —
        // никаких сетевых вызовов тут не происходит. Реальные RPC полетят потом, и не к мастеру, а к data-node'ам — когда дёрнут GetReadyEvent() и Read().
        Reader_ = CreateAppropriateSchemalessMultiChunkReader(
            tableReaderOptions,
            tableReaderConfig,
            // @gearonixx TChunkReaderHost — "контекст" для chunk-ридеров:
            // клиент (через него ходим в мастер/ноды), троттлер пропускной
            // способности (bytes/s) и троттлер RPS (запросов в секунду).
            New<TChunkReaderHost>(
                Client_,
                MakeUniformPerCategoryThrottlerProvider(BandwidthThrottler_),
                RpsThrottler_),
            tableReadSpec,
            chunkReadOptions,
            Options_.Unordered,
            NameTable_,
            ColumnFilter_);

        YT_LOG_DEBUG("@@gearonixx_driver Schemaless multi-chunk reader created, waiting for its ReadyEvent");
        // @gearonixx Сам Create... возвращает ридер сразу, но первичная инициализация
        // (открытие первого чанка, получение StartRowIndex и т. п.) асинхронная.
        // WaitFor паркует текущее fiber'у, пока future не завершится, и затем
        // ThrowOnError кидает исключение, если внутри что-то упало.
        WaitFor(Reader_->GetReadyEvent())
            .ThrowOnError();

        // @gearonixx Индекс первой строки, которую отдаст нижележащий ридер
        // (с учётом ranges/lower_limit из RichPath). Запоминаем для GetStartRowIndex().
        StartRowIndex_ = Reader_->GetTableRowIndex();
        YT_LOG_DEBUG("@@gearonixx_driver Underlying reader is ready (StartRowIndex: %v)", StartRowIndex_);

        // @gearonixx Если читаем в рамках транзакции — подписываемся на её abort
        // через TTransactionListener. После этого IsAborted() начнёт возвращать
        // true, как только транзакция упадёт, и Read() сразу отдаст пустой батч.
        if (Transaction_) {
            StartListenTransaction(Transaction_);
            YT_LOG_DEBUG("@@gearonixx_driver Started listening transaction (TransactionId: %v)", TransactionId_);
        }

        // @gearonixx Жёсткий дедлайн на всё чтение целиком (а не на один Read).
        // CpuInstant — это значение rdtsc-подобного счётчика, дешёвое для сравнения
        // в hot path; в Read() мы только сравниваем GetCpuInstant() > ReadDeadline_.
        if (Config_->MaxReadDuration) {
            ReadDeadline_ = NProfiling::GetCpuInstant() + NProfiling::DurationToCpuDuration(*Config_->MaxReadDuration);
            YT_LOG_DEBUG("@@gearonixx_driver Read deadline set (MaxReadDuration: %v)", Config_->MaxReadDuration);
        }

        // @gearonixx Дошли сюда без исключений → ReadyEvent_ зарезолвится в OK,
        // и внешний код, ждавший CreateTableReader().Apply(...), получит ридер.
        YT_LOG_DEBUG("@@gearonixx_driver TTableReader::DoOpen finished successfully");
    }
};

////////////////////////////////////////////////////////////////////////////////

TFuture<ITableReaderPtr> CreateTableReader(
    IClientPtr client,
    const NYPath::TRichYPath& path,
    const TTableReaderOptions& options,
    TNameTablePtr nameTable,
    const TColumnFilter& columnFilter,
    IThroughputThrottlerPtr bandwidthThrottler,
    IThroughputThrottlerPtr rpsThrottler,
    IMemoryUsageTrackerPtr memoryUsageTracker)
{
    const auto& Logger = ApiLogger();
    YT_LOG_DEBUG("@@gearonixx_driver NNative::CreateTableReader factory invoked (Path: %v, TransactionId: %v)",
        path,
        options.TransactionId);

    NApi::ITransactionPtr transaction;
    if (options.TransactionId) {
        TTransactionAttachOptions transactionOptions;
        transactionOptions.Ping = options.Ping;
        transactionOptions.PingAncestors = options.PingAncestors;
        transaction = client->AttachTransaction(options.TransactionId, transactionOptions);
        YT_LOG_DEBUG("@@gearonixx_driver Attached transaction (TransactionId: %v, Ping: %v, PingAncestors: %v)",
            options.TransactionId,
            options.Ping,
            options.PingAncestors);
    } else {
        YT_LOG_DEBUG("@@gearonixx_driver No transaction attached (TransactionId is empty)");
    }

    YT_LOG_DEBUG("@@gearonixx_driver Constructing TTableReader instance (will trigger DoOpen via reader-invoker)");
    auto reader = New<TTableReader>(
        options.Config ? options.Config : New<TTableReaderConfig>(),
        options,
        client,
        transaction,
        path,
        nameTable,
        columnFilter,
        bandwidthThrottler,
        rpsThrottler,
        std::move(memoryUsageTracker));
    YT_LOG_DEBUG("@@gearonixx_driver TTableReader constructed, DoOpen scheduled on chunk-reader invoker");

    return reader->GetReadyEvent().Apply(BIND([=] () -> ITableReaderPtr {
        YT_LOG_DEBUG("@@gearonixx_driver TTableReader ReadyEvent resolved (DoOpen finished), returning reader");
        return reader;
    }));
}

////////////////////////////////////////////////////////////////////////////////

class TSchemalessMultiChunkReaderAdapter
    : public IRowBatchReader
{
public:
    TSchemalessMultiChunkReaderAdapter(ISchemalessMultiChunkReaderPtr reader)
        : Reader_(std::move(reader))
    { }

    IUnversionedRowBatchPtr Read(const TRowBatchReadOptions& options) override
    {
        return Reader_->Read(options);
    }

    const TNameTablePtr& GetNameTable() const override
    {
        return Reader_->GetNameTable();
    }

    TFuture<void> GetReadyEvent() const override
    {
        return Reader_->GetReadyEvent();
    }

private:
    const ISchemalessMultiChunkReaderPtr Reader_;
};

IRowBatchReaderPtr ToApiRowBatchReader(ISchemalessMultiChunkReaderPtr reader)
{
    return New<TSchemalessMultiChunkReaderAdapter>(std::move(reader));
}

NTableClient::TTableReaderOptionsPtr ToInternalTableReaderOptions(const TTableReaderOptions& options)
{
    auto result = New<NTableClient::TTableReaderOptions>();
    result->EnableTableIndex = options.EnableTableIndex;
    result->EnableRangeIndex = options.EnableRangeIndex;
    result->EnableRowIndex = options.EnableRowIndex;
    result->EnableTabletIndex = options.EnableTabletIndex;
    result->EnableAnyUnpacking = options.EnableAnyUnpacking;
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
