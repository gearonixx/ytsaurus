#pragma once

#include "command.h"

#include <yt/yt/client/formats/format.h>

#include <yt/yt/client/table_client/config.h>
#include <yt/yt/client/table_client/unversioned_row.h>

#include <yt/yt/client/ypath/rich.h>

namespace NYT::NDriver {

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `read_table` — выгружает строки статической таблицы (или ranges/columns в TRichYPath)
// @gearonixx клиенту: открывает ITableReader через NApi, гонит чанки через указанный формат (yson/json/...)
// @gearonixx в output stream драйвера. Поддерживает control attributes и неупорядоченное чтение.
class TReadTableCommand
    : public TTypedCommand<NApi::TTableReaderOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TReadTableCommand);

    static void Register(TRegistrar registrar);

private:
  // Path — TRichYPath, путь к таблице плюс «обвес» (ranges, columns, transaction_id и т.п.), то есть что и как читать.
  // TableReader — узел YSON с конфигом ридера (буфера, ретраи, размер чанков); приходит из параметров запроса и накладывается поверх дефолтов драйвера.
  //
  // ControlAttributes — настройка, какие служебные поля (row_index, range_index, tablet_index, key_switch) подмешивать в выходной поток рядом с данными строки.
  // Unordered — разрешить читать чанки параллельно без сохранения порядка строк; быстрее, но порядок строк в выходе не гарантируется.
  // StartRowIndexOnly — выдать только индекс первой строки каждого range и не читать сами данные; используется, когда клиенту нужны только границы, а не содержимое.
    NYPath::TRichYPath Path;
    NYTree::INodePtr TableReader;
    NFormats::TControlAttributesConfigPtr ControlAttributes;
    bool Unordered;
    bool StartRowIndexOnly;

    void DoExecute(ICommandContextPtr context) override;
    bool HasResponseParameters() const override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `read_blob_table` — читает таблицу как один (или несколько) blob-ов:
// @gearonixx строки сортированы по part_index, данные склеиваются из колонки DataColumnName, начиная со
// @gearonixx StartPartIndex+Offset; используется для хранения больших файлов поверх таблиц.
class TReadBlobTableCommand
    : public TTypedCommand<NApi::TTableReaderOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TReadBlobTableCommand);

    static void Register(TRegistrar registrar);

private:
    NYPath::TRichYPath Path;
    NYTree::INodePtr TableReader;

    std::optional<std::string> PartIndexColumnName;
    std::optional<std::string> DataColumnName;

    i64 StartPartIndex;
    i64 Offset;
    i64 PartSize;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `read_table_partition` — читает одну партицию по cookie, выданному заранее
// @gearonixx командой partition_tables; нужна для параллельного чтения большого набора таблиц
// @gearonixx несколькими воркерами.
class TReadTablePartitionCommand
    : public TTypedCommand<NApi::TReadTablePartitionOptions>
{
    REGISTER_YSON_STRUCT_LITE(TReadTablePartitionCommand);

    static void Register(TRegistrar registrar);

private:
    std::string Cookie;
    NFormats::TControlAttributesConfigPtr ControlAttributes;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `locate_skynet_share` — отдаёт локацию чанков таблицы для раздачи через Skynet
// @gearonixx (внутренний P2P-CDN Яндекса); таблица должна быть в специальном sorted-blob-формате.
class TLocateSkynetShareCommand
    : public TTypedCommand<NApi::TLocateSkynetShareOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TLocateSkynetShareCommand);

    static void Register(TRegistrar registrar);

private:
    NYPath::TRichYPath Path;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `write_table` — пишет строки из input stream драйвера в (статическую) таблицу.
// @gearonixx Открывает ITableWriter, конвертирует входной формат в unversioned rows, буферизует и
// @gearonixx коммитит транзакцией; перед записью при необходимости создаёт таблицу.
class TWriteTableCommand
    : public TTypedCommand<NApi::TTableWriterOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TWriteTableCommand);

    static void Register(TRegistrar registrar);

protected:
    virtual NApi::ITableWriterPtr CreateTableWriter(
        const ICommandContextPtr& context);

    void DoExecuteImpl(const ICommandContextPtr& context);

private:
    NYPath::TRichYPath Path;
    NYTree::INodePtr TableWriter;
    i64 MaxRowBufferSize;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `get_table_columnar_statistics` — считает статистики по колонкам (size, weight,
// @gearonixx data weight) набора таблиц, не читая сами данные; полезно для планировщиков map-reduce.
class TGetTableColumnarStatisticsCommand
    : public TTypedCommand<NApi::TGetColumnarStatisticsOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TGetTableColumnarStatisticsCommand);

    static void Register(TRegistrar registrar);

private:
    std::vector<NYPath::TRichYPath> Paths;
    NTableClient::EColumnarStatisticsFetcherMode FetcherMode;
    std::optional<int> MaxChunksPerNodeFetch;
    bool EnableEarlyFinish;
    bool EnableReadSizeEstimation;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `partition_tables` — режет набор таблиц на партиции по data weight или числу
// @gearonixx партиций; при EnableCookies возвращает cookies, которые потом скармливаются
// @gearonixx read_table_partition для параллельного чтения.
class TPartitionTablesCommand
    : public TTypedCommand<NApi::TPartitionTablesOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TPartitionTablesCommand);

    static void Register(TRegistrar registrar);

private:
    std::vector<NYPath::TRichYPath> Paths;
    NTableClient::ETablePartitionMode PartitionMode;
    std::optional<i64> DataWeightPerPartition;
    std::optional<i64> CompressedDataSizePerPartition;
    std::optional<int> MaxPartitionCount;
    bool EnableKeyGuarantee;

    // TODO(pavook): remove or rename this option, as semantically it's
    // really nothing more than AreYouReallySureYouWantMaxPartitionCount.
    //! Treat the #DataWeightPerPartition as a hint and not as a maximum limit.
    //! Consider the situation when the #MaxPartitionCount is given
    //! and the total data weight exceeds #MaxPartitionCount * #DataWeightPerPartition.
    //! If #AdjustDataWeightPerPartition is |true|
    //! the #partition_tables command will yield partitions exceeding the #DataWeightPerPartition.
    //! If #AdjustDataWeightPerPartition is |false|
    //! the #partition_tables command will throw an exception.
    bool AdjustDataWeightPerPartition;

    //! Return cookies that can be used with read_table_partition command.
    bool EnableCookies;
    //! Whether to include node descriptors in the cookie (effective only when EnableCookies is true).
    //! Increases cookie size but likely reduces read latency with read_table_partition command.
    bool FetchCookieNodeDescriptors;

    bool OmitInaccessibleRows;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx шаблон-база для всех команд над dynamic-таблицей, которые оперируют диапазоном tablet-ов:
// @gearonixx даёт общий параметр Path и опции FirstTabletIndex/LastTabletIndex.
template <class TOptions>
class TTabletCommandBase
    : public TTypedCommand<TOptions>
{
protected:
    NYPath::TRichYPath Path;

    REGISTER_YSON_STRUCT_LITE(TTabletCommandBase);

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("path", &TThis::Path);

        registrar.template ParameterWithUniversalAccessor<std::optional<int>>(
            "first_tablet_index",
            [] (TThis* command) -> auto& {
                return command->Options.FirstTabletIndex;
            })
            .Default();

        registrar.template ParameterWithUniversalAccessor<std::optional<int>>(
            "last_tablet_index",
            [] (TThis* command) -> auto& {
                return command->Options.LastTabletIndex;
            })
            .Default();
    }
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `mount_table` — переводит tablets из unmounted в mounted: tablet cell поднимает
// @gearonixx их в память и начинает обслуживать lookup/select/insert; обязательно для dynamic-таблиц.
class TMountTableCommand
    : public TTabletCommandBase<NApi::TMountTableOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TMountTableCommand);

    static void Register(TRegistrar registrar);

private:
    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `unmount_table` — обратное к mount: tablet cell флашит dynamic stores на диск,
// @gearonixx освобождает память, перестаёт обслуживать запросы. Force=true пропускает flush.
class TUnmountTableCommand
    : public TTabletCommandBase<NApi::TUnmountTableOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TUnmountTableCommand);

    static void Register(TRegistrar registrar);

private:
    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `remount_table` — применяет новые mount-атрибуты (mount config) без полного
// @gearonixx unmount/mount цикла; данные не сбрасываются, просто пересоздаются runtime-структуры.
class TRemountTableCommand
    : public TTabletCommandBase<NApi::TRemountTableOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TRemountTableCommand);

    static void Register(TRegistrar /*registrar*/)
    { }

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `freeze_table` — переводит tablets в frozen-состояние (read-only, без dynamic
// @gearonixx stores в памяти); используется как промежуточный шаг перед backup-ом или для экономии RAM.
class TFreezeTableCommand
    : public TTabletCommandBase<NApi::TFreezeTableOptions>
{
    REGISTER_YSON_STRUCT_LITE(TFreezeTableCommand);

    static void Register(TRegistrar /*registrar*/)
    { }

private:
    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `unfreeze_table` — возвращает frozen tablets обратно в обычное mounted-состояние,
// @gearonixx снова разрешая запись.
class TUnfreezeTableCommand
    : public TTabletCommandBase<NApi::TUnfreezeTableOptions>
{
    REGISTER_YSON_STRUCT_LITE(TUnfreezeTableCommand);

    static void Register(TRegistrar /*registrar*/)
    { }

public:
    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `cancel_tablet_transition` — отменяет уже стартовавший, но ещё не завершившийся
// @gearonixx переход состояния (mount/unmount/freeze/...) одного tablet-а по его TTabletId.
class TCancelTabletTransitionCommand
    : public TTypedCommand<NApi::TCancelTabletTransitionOptions>
{
    NTabletClient::TTabletId TabletId;

    REGISTER_YSON_STRUCT_LITE(TCancelTabletTransitionCommand);

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("tablet_id", &TThis::TabletId);
    }

public:
    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `reshard_table` — меняет разбиение dynamic-таблицы на tablet-ы: задаётся либо
// @gearonixx список PivotKeys (для sorted), либо TabletCount (для ordered); таблица должна быть
// @gearonixx unmounted на затрагиваемом диапазоне.
class TReshardTableCommand
    : public TTabletCommandBase<NApi::TReshardTableOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TReshardTableCommand);

    static void Register(TRegistrar registrar);

private:
    std::optional<std::vector<NTableClient::TLegacyOwningKey>> PivotKeys;
    std::optional<int> TabletCount;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `reshard_table_automatic` — просит мастер автоматически пересчитать pivot keys
// @gearonixx по статистикам chunks и выполнить reshard; ручные PivotKeys/TabletCount не нужны.
class TReshardTableAutomaticCommand
    : public TTabletCommandBase<NApi::TReshardTableAutomaticOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TReshardTableAutomaticCommand);

    static void Register(TRegistrar registrar);

private:
    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `alter_table` — меняет атрибуты таблицы: schema, dynamic↔static, upstream_replica_id,
// @gearonixx schema_modification и т.п. Не трогает данные, только метаданные на мастере.
class TAlterTableCommand
    : public TTypedCommand<NApi::TAlterTableOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TAlterTableCommand);

    static void Register(TRegistrar registrar);

private:
    NYPath::TRichYPath Path;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

struct TSelectRowsOptions
    : public NApi::TSelectRowsOptions
    , public TTabletTransactionOptions
{ };

// @gearonixx команда `select_rows` — выполняет SQL-подобный запрос (YT-QL) по dynamic-таблицам;
// @gearonixx сервер разбирает Query, строит план, опрашивает tablet cells параллельно и стримит
// @gearonixx результат клиенту.
class TSelectRowsCommand
    : public TTypedCommand<TSelectRowsOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TSelectRowsCommand);

    static void Register(TRegistrar registrar);

private:
    TString Query;
    NYTree::IMapNodePtr PlaceholderValues;
    bool EnableStatistics = false;

    void DoExecute(ICommandContextPtr context) override;
    bool HasResponseParameters() const override;
};

////////////////////////////////////////////////////////////////////////////////

struct TExplainQueryOptions
    : public NApi::TExplainQueryOptions
    , public TTabletTransactionOptions
{ };

// @gearonixx команда `explain_query` — возвращает план запроса YT-QL без его выполнения;
// @gearonixx используется для отладки производительности select_rows.
class TExplainQueryCommand
    : public TTypedCommand<TExplainQueryOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TExplainQueryCommand);

    static void Register(TRegistrar registrar);

private:
    TString Query;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `insert_rows` — пишет строки в dynamic-таблицу в рамках tablet-транзакции;
// @gearonixx Update=true делает merge по ключу вместо overwrite, Aggregate включает aggregate-колонки,
// @gearonixx LockType задаёт тип блокировки (shared/exclusive/...).
class TInsertRowsCommand
    : public TTypedCommand<TInsertRowsOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TInsertRowsCommand);

    static void Register(TRegistrar registrar);

private:
    NYTree::INodePtr TableWriter;
    NYPath::TRichYPath Path;
    bool Update;
    bool Aggregate;
    NTableClient::ELockType LockType;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

struct TLookupRowsOptions
    : public NApi::TLookupRowsOptions
    , public TTabletTransactionOptions
{ };

// @gearonixx команда `lookup_rows` — точечная выборка строк sorted dynamic-таблицы по полному ключу;
// @gearonixx быстрее select для key-based доступа. Versioned=true возвращает все версии (с timestamps),
// @gearonixx RetentionConfig ограничивает сколько именно версий вернуть.
class TLookupRowsCommand
    : public TTypedCommand<TLookupRowsOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TLookupRowsCommand);

    static void Register(TRegistrar registrar);

private:
    NYTree::INodePtr TableWriter;
    NYPath::TRichYPath Path;
    std::optional<std::vector<std::string>> ColumnNames;
    bool Versioned;
    NTableClient::TRetentionConfigPtr RetentionConfig;

    void DoExecute(ICommandContextPtr context) override;
    bool HasResponseParameters() const override;
};

////////////////////////////////////////////////////////////////////////////////

struct TPullRowsOptions
    : public NApi::TPullRowsOptions
{ };

// @gearonixx команда `pull_rows` — забирает поток изменений (changelog) реплицируемой таблицы начиная
// @gearonixx с указанного timestamp/row-index; используется replicator-ом для асинхронной репликации.
class TPullRowsCommand
    : public TTypedCommand<TPullRowsOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TPullRowsCommand);

    static void Register(TRegistrar registrar);

private:
    NYPath::TRichYPath Path;

    void DoExecute(ICommandContextPtr context) override;
    bool HasResponseParameters() const override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `get_in_sync_replicas` — для replicated-таблицы возвращает список реплик,
// @gearonixx синхронных на конкретные ключи (или на все, при AllKeys=true) на момент TS; нужна, чтобы
// @gearonixx клиент мог сделать консистентный lookup из реплики.
class TGetInSyncReplicasCommand
    : public TTypedCommand<NApi::TGetInSyncReplicasOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TGetInSyncReplicasCommand);

    static void Register(TRegistrar registrar);

private:
    NYPath::TRichYPath Path;
    bool AllKeys;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `delete_rows` — удаляет строки dynamic-таблицы по ключам в рамках tablet-транзакции.
class TDeleteRowsCommand
    : public TTypedCommand<TDeleteRowsOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TDeleteRowsCommand);

    static void Register(TRegistrar registrar);

private:
    NYTree::INodePtr TableWriter;
    NYPath::TRichYPath Path;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `lock_rows` — берёт row-level блокировки указанных типов на ключах без записи
// @gearonixx данных; используется для координации параллельных транзакций над одной строкой.
class TLockRowsCommand
    : public TTypedCommand<TLockRowsOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TLockRowsCommand);

    static void Register(TRegistrar registrar);

private:
    NYTree::INodePtr TableWriter;
    NYPath::TRichYPath Path;
    std::vector<std::string> Locks;
    NTableClient::ELockType LockType;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `trim_rows` — для ordered dynamic-таблицы обрезает начало конкретного tablet-а
// @gearonixx до TrimmedRowCount; используется для TTL-подобной чистки очередей.
class TTrimRowsCommand
    : public TTypedCommand<NApi::TTrimTableOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TTrimRowsCommand);

    static void Register(TRegistrar registrar);

private:
    NYPath::TRichYPath Path;
    int TabletIndex;
    i64 TrimmedRowCount;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `enable_table_replica` — включает реплику replicated-таблицы по её ReplicaId.
class TEnableTableReplicaCommand
    : public TTypedCommand<NApi::TAlterTableReplicaOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TEnableTableReplicaCommand);

    static void Register(TRegistrar registrar);

private:
    NTabletClient::TTableReplicaId ReplicaId;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `disable_table_replica` — выключает реплику replicated-таблицы.
class TDisableTableReplicaCommand
    : public TTypedCommand<NApi::TAlterTableReplicaOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TDisableTableReplicaCommand);

    static void Register(TRegistrar registrar);

private:
    NTabletClient::TTableReplicaId ReplicaId;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `alter_table_replica` — меняет произвольные атрибуты реплики (mode sync/async,
// @gearonixx enabled, preserve_timestamps, atomicity и т.п.).
class TAlterTableReplicaCommand
    : public TTypedCommand<NApi::TAlterTableReplicaOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TAlterTableReplicaCommand);

    static void Register(TRegistrar registrar);

private:
    NTabletClient::TTableReplicaId ReplicaId;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `get_tablet_infos` — возвращает рантайм-информацию по указанным tablet-ам
// @gearonixx (total_row_count, trimmed_row_count, последние записанные TS, replication progress).
class TGetTabletInfosCommand
    : public TTypedCommand<NApi::TGetTabletInfosOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TGetTabletInfosCommand);

    static void Register(TRegistrar registrar);

private:
    NYPath::TYPath Path;
    std::vector<int> TabletIndexes;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `get_tablet_errors` — собирает ошибки tablet-ов таблицы (background flush/compaction,
// @gearonixx replication errors); используется в мониторинге health-а.
class TGetTabletErrorsCommand
    : public TTypedCommand<NApi::TGetTabletErrorsOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TGetTabletErrorsCommand);

    static void Register(TRegistrar registrar);

private:
    NYPath::TYPath Path;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `get_table_pivot_keys` — возвращает список pivot keys таблицы (границы tablet-ов);
// @gearonixx нужна для построения параллельного reader-а / sanity-чек reshard-а.
class TGetTablePivotKeysCommand
    : public TTypedCommand<NApi::TGetTablePivotKeysOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TGetTablePivotKeysCommand);

    static void Register(TRegistrar registrar);

private:
    NYPath::TYPath Path;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `create_table_backup` — создаёт consistent backup по манифесту (набор пар
// @gearonixx source→destination таблиц); атомарно фризит источники, делает дешёвый copy метаданных.
class TCreateTableBackupCommand
    : public TTypedCommand<NApi::TCreateTableBackupOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TCreateTableBackupCommand);

    static void Register(TRegistrar registrar);

private:
    NApi::TBackupManifestPtr Manifest;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx команда `restore_table_backup` — обратная к create_table_backup: восстанавливает таблицы
// @gearonixx из backup-копий, описанных манифестом.
class TRestoreTableBackupCommand
    : public TTypedCommand<NApi::TRestoreTableBackupOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TRestoreTableBackupCommand);

    static void Register(TRegistrar registrar);

private:
    NApi::TBackupManifestPtr Manifest;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

struct TGetTableMountInfoCommandOptions
{ };

// @gearonixx команда `get_table_mount_info` — отдаёт клиенту mount-info таблицы: список tablet-ов,
// @gearonixx их cell-id, peers, schema; клиентская библиотека по этой инфе ходит напрямую в tablet
// @gearonixx cells, минуя rpc-proxy.
class TGetTableMountInfoCommand
    : public TTypedCommand<TGetTableMountInfoCommandOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TGetTableMountInfoCommand);

    static void Register(TRegistrar registrar);

private:
    NYTree::TYPath Path_;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

struct TGetTableRowCountCommandOptions
{ };

class TGetTableRowCountCommand
    : public TTypedCommand<TGetTableRowCountCommandOptions>
{
public:
    REGISTER_YSON_STRUCT_LITE(TGetTableRowCountCommand);

    static void Register(TRegistrar registrar);

private:
    NYPath::TYPath Path;

    void DoExecute(ICommandContextPtr context) override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDriver
