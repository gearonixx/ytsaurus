#include "adapters.h"

#include "private.h"
#include "schema.h"
#include "row_batch.h"
#include "unversioned_row.h"

#include <yt/yt/client/api/table_writer.h>

#include <yt/yt/core/concurrency/scheduler.h>
#include <yt/yt/core/concurrency/throughput_throttler.h>
#include <yt/yt/core/concurrency/periodic_yielder.h>

namespace NYT::NTableClient {

using namespace NApi;
using namespace NConcurrency;
using namespace NCrypto;
using namespace NFormats;

using NProfiling::TWallTimer;

////////////////////////////////////////////////////////////////////////////////

constinit const auto Logger = TableClientLogger;

////////////////////////////////////////////////////////////////////////////////

template <std::derived_from<ITableWriter> TInterface, std::derived_from<IUnversionedWriter> TUnderlyingInterface>
class TApiFromSchemalessWriterAdapter
    : public TInterface
{
public:
    explicit TApiFromSchemalessWriterAdapter(TIntrusivePtr<TUnderlyingInterface> underlyingWriter)
        : UnderlyingWriter_(std::move(underlyingWriter))
    { }

    bool Write(TRange<TUnversionedRow> rows) /*override*/
    {
        return UnderlyingWriter_->Write(rows);
    }

    TFuture<void> GetReadyEvent() /*override*/
    {
        return UnderlyingWriter_->GetReadyEvent();
    }

    TFuture<void> Close() /*override*/
    {
        return UnderlyingWriter_->Close();
    }

    const TNameTablePtr& GetNameTable() const /*override*/
    {
        return UnderlyingWriter_->GetNameTable();
    }

    const TTableSchemaPtr& GetSchema() const /*override*/
    {
        return UnderlyingWriter_->GetSchema();
    }

protected:
    const TIntrusivePtr<TUnderlyingInterface> UnderlyingWriter_;
};

ITableWriterPtr CreateApiFromSchemalessWriterAdapter(
    IUnversionedWriterPtr underlyingWriter)
{
    return New<TApiFromSchemalessWriterAdapter<ITableWriter, IUnversionedWriter>>(std::move(underlyingWriter));
}

////////////////////////////////////////////////////////////////////////////////

class TApiFromSchemalessTableFragmentWriterAdapter
    : public TApiFromSchemalessWriterAdapter<ITableFragmentWriter, IUnversionedTableFragmentWriter>
{
public:
    using TBase = TApiFromSchemalessWriterAdapter<ITableFragmentWriter, IUnversionedTableFragmentWriter>;
    using TBase::TBase;

    TSignedWriteFragmentResultPtr GetWriteFragmentResult() const /*override*/
    {
        return TBase::UnderlyingWriter_->GetWriteFragmentResult();
    }
};

ITableFragmentWriterPtr CreateApiFromSchemalessWriterAdapter(
    IUnversionedTableFragmentWriterPtr underlyingWriter)
{
    return New<TApiFromSchemalessTableFragmentWriterAdapter>(std::move(underlyingWriter));
}

////////////////////////////////////////////////////////////////////////////////

class TSchemalessApiFromWriterAdapter
    : public IUnversionedWriter
{
public:
    TSchemalessApiFromWriterAdapter(
        IRowBatchWriterPtr underlyingWriter,
        TTableSchemaPtr schema)
        : UnderlyingWriter_(std::move(underlyingWriter))
        , Schema_(std::move(schema))
    { }

    bool Write(TRange<TUnversionedRow> rows) override
    {
        return UnderlyingWriter_->Write(rows);
    }

    TFuture<void> GetReadyEvent() override
    {
        return UnderlyingWriter_->GetReadyEvent();
    }

    TFuture<void> Close() override
    {
        return UnderlyingWriter_->Close();
    }

    const TNameTablePtr& GetNameTable() const override
    {
        return UnderlyingWriter_->GetNameTable();
    }

    const TTableSchemaPtr& GetSchema() const override
    {
        return Schema_;
    }

    std::optional<TRowsDigest> GetDigest() const override
    {
        return std::nullopt;
    }

private:
    const IRowBatchWriterPtr UnderlyingWriter_;
    const TTableSchemaPtr Schema_;
};

IUnversionedWriterPtr CreateSchemalessFromApiWriterAdapter(
    IRowBatchWriterPtr underlyingWriter)
{
    return New<TSchemalessApiFromWriterAdapter>(std::move(underlyingWriter), New<TTableSchema>());
}

IUnversionedWriterPtr CreateSchemalessFromApiWriterAdapter(
    ITableWriterPtr underlyingWriter)
{
    return New<TSchemalessApiFromWriterAdapter>(underlyingWriter, underlyingWriter->GetSchema());
}

////////////////////////////////////////////////////////////////////////////////

void PipeReaderToWriter(
    const IRowBatchReaderPtr& reader,
    const IUnversionedRowsetWriterPtr& writer,
    const TPipeReaderToWriterOptions& options)
{
    auto yielder = CreatePeriodicYielder(TDuration::Seconds(1));

    TRowBatchReadOptions readOptions{
        .MaxRowsPerRead = options.BufferRowCount,
        .MaxDataWeightPerRead = options.BufferDataWeight
    };
    while (auto batch = reader->Read(readOptions)) {
        yielder.TryYield();

        TSharedRange<TUnversionedRow> rows;

        try {
            if (batch->IsEmpty()) {
                WaitFor(reader->GetReadyEvent())
                    .ThrowOnError();
                continue;
            }

            rows = batch->MaterializeRows();

            if (options.ValidateValues) {
                for (auto row : rows) {
                    for (const auto& value : row) {
                        ValidateStaticValue(value);
                    }
                }
            }

            if (options.Throttler) {
                i64 dataWeight = 0;
                for (auto row : rows) {
                    dataWeight += GetDataWeight(row);
                }
                WaitFor(options.Throttler->Throttle(dataWeight))
                    .ThrowOnError();
            }

            if (!rows.empty() && options.PipeDelay) {
                TDelayedExecutor::WaitForDuration(options.PipeDelay);
            }
        } catch (const std::exception& ex) {
            if (options.ReaderErrorWrapper) {
                THROW_ERROR options.ReaderErrorWrapper(ex);
            } else {
                throw;
            }
        }

        if (!writer->Write(rows)) {
            WaitFor(writer->GetReadyEvent())
                .ThrowOnError();
        }
    }

    WaitFor(writer->Close())
        .ThrowOnError();
}


    // 1. HTTP-прокси на запрос read-table зовёт client->CreateTableReader(path, options). Возвращается ITableReader — это и есть тот «reader», который потом оборачивается и попадает в PipeReaderToWriterByBatches.
    // 2. Под капотом ITableReader идёт в мастер (Cypress) и спрашивает: «что такое //home/input_table?» — мастер отвечает схемой + списком чанков (chunk IDs) и репликами (на каких data-нодах эти чанки лежат).
    // 3. Дальше reader открывает RPC-стрим к data-нодам и качает чанки кусками. Чанк на ноде — это файл на диске в её сторе; внутри он в колоночном/блочном формате YT, со сжатием.
    // 4. Reader декодирует блоки чанка → распаковывает → разбирает в TUnversionedRow → складывает в батч → отдаёт его наружу через Read().
    //
    // То есть reader->Read не «лезет в таблицу» сам — он отдаёт уже подготовленный батч из своего внутреннего буфера, который фоном наполняется чанками, прилетающими по RPC с data-нод. GetReadyEvent() как раз и ждётся,
    // когда буфер пуст, а следующий батч ещё едет по сети.

void PipeReaderToWriterByBatches(
    const IRowBatchReaderPtr& reader,
    const ISchemalessFormatWriterPtr& writer,
    TRowBatchReadOptions options,
    TCallback<void(TRowBatchReadOptions* mutableOptions, TDuration timeForBatch)> optionsUpdater,
    TDuration pipeDelay)
{
    try {
        auto yielder = CreatePeriodicYielder(TDuration::Seconds(1));

        //  reader->Read выдаст батч (IUnversionedRowBatch), внутри которого две TUnversionedRow.
  //       ow 0: [ {Id=0 (column "id"),   Type=Int64,  Data=0},
  //          {Id=1 (column "text"), Type=String, Data="Hello"} ]
  // Row 1: [ {Id=0, Type=Int64,  Data=1},
  //          {Id=1, Type=String, Data="World!"} ]


        // @@CLAUDE_LOG_HERE
        int batchIndex = 0;
        for (bool isFirstBatch = true; auto batch = reader->Read(options); isFirstBatch = false) {
            yielder.TryYield();

            YT_LOG_DEBUG("@gearonixx === reader->Read returned (BatchIndex: %v, RowCount: %v, IsEmpty: %v, MaxRowsPerRead: %v)",
                batchIndex,
                batch->GetRowCount(),
                batch->IsEmpty(),
                options.MaxRowsPerRead);

            if (batch->IsEmpty()) {
                YT_LOG_DEBUG("@gearonixx batch is empty, waiting for GetReadyEvent (BatchIndex: %v)", batchIndex);
                WaitFor(reader->GetReadyEvent())
                    .ThrowOnError();
                ++batchIndex;
                continue;
            }

            {
                auto rows = batch->MaterializeRows();
                for (int rowIdx = 0; rowIdx < std::ssize(rows); ++rowIdx) {
                    auto row = rows[rowIdx];
                    YT_LOG_DEBUG("@gearonixx   Row %v: ValueCount=%v, Repr=%v",
                        rowIdx,
                        static_cast<int>(row.GetCount()),
                        row);
                    for (const auto& value : row) {
                        YT_LOG_DEBUG("@gearonixx     Value: Id=%v, Type=%v, Flags=%v",
                            value.Id,
                            value.Type,
                            value.Flags);
                    }
                }
            }
            ++batchIndex;

            auto rowsRead = batch->GetRowCount();

            if (pipeDelay != TDuration::Zero()) {
                TDelayedExecutor::WaitForDuration(pipeDelay);
            }

            TWallTimer timer(/*start*/ false);

            if (optionsUpdater) {
                timer.Start();
            }

            if (!writer->WriteBatch(batch)) {
                WaitFor(writer->GetReadyEvent())
                    .ThrowOnError();
            }

            if (optionsUpdater && !isFirstBatch) {
                options.MaxRowsPerRead = rowsRead;
                optionsUpdater(&options, timer.GetElapsedTime());
            }
        }

        WaitFor(writer->Close())
            .ThrowOnError();
    } catch (const std::exception& ex) {
        YT_LOG_ERROR(ex, "Failed to transfer batches from reader to writer");

        throw;
    }
}

i64 PipeInputToOutput(
    IInputStream* input,
    IOutputStream* output,
    i64 bufferBlockSize)
{
    i64 totalBytes = 0;

    struct TWriteBufferTag { };
    TBlob buffer(GetRefCountedTypeCookie<TWriteBufferTag>(), bufferBlockSize, /*initializeStorage*/ false);

    auto yielder = CreatePeriodicYielder(TDuration::Seconds(1));

    while (true) {
        yielder.TryYield();

        size_t length = input->Read(buffer.Begin(), buffer.Size());
        if (length == 0) {
            break;
        }

        totalBytes += length;

        output->Write(buffer.Begin(), length);
    }

    output->Finish();

    return totalBytes;
}

i64 PipeInputToOutput(
    const IAsyncInputStreamPtr& input,
    IOutputStream* output,
    i64 bufferBlockSize)
{
    i64 totalBytes = 0;

    struct TWriteBufferTag { };
    auto buffer = TSharedMutableRef::Allocate<TWriteBufferTag>(bufferBlockSize, {.InitializeStorage = false});

    while (true) {
        auto length = WaitFor(input->Read(buffer))
            .ValueOrThrow();

        if (length == 0) {
            break;
        }

        totalBytes += length;

        output->Write(buffer.Begin(), length);
    }

    output->Finish();

    return totalBytes;
}

i64 PipeInputToOutput(
    const IAsyncZeroCopyInputStreamPtr& input,
    IOutputStream* output)
{
    i64 totalBytes = 0;

    while (true) {
        auto data = WaitFor(input->Read())
            .ValueOrThrow();

        if (!data) {
            break;
        }

        totalBytes += data.Size();

        output->Write(data.Begin(), data.Size());
    }

    output->Finish();

    return totalBytes;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
