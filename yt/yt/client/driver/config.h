#pragma once

#include "public.h"

#include <yt/yt/client/api/public.h>

#include <yt/yt/client/api/rpc_proxy/public.h>

#include <yt/yt/client/table_client/public.h>

#include <yt/yt/client/chunk_client/public.h>

#include <yt/yt/core/ytree/yson_struct.h>

namespace NYT::NDriver {

////////////////////////////////////////////////////////////////////////////////

constexpr int ApiVersion3 = 3;
constexpr int ApiVersion4 = 4;

// @gearonixx
// Это конфиги для разных «движков» внутри драйвера — драйвер ведь не просто коннектор к YT, а универсальный исполнитель команд, и каждая команда требует своих настроек.

struct TDriverConfig
    : public NYTree::TYsonStruct
{
    // @gearonixx
    // wtf???
    // настройки для команд работы с файлами в Cypress (как читать/писать большие бинарные файлы по чанкам).
    NApi::TFileReaderConfigPtr FileReader;
    NApi::TFileWriterConfigPtr FileWriter;

    //  для табличных команд; тут много специфики: формат строк, схема, политика sorted-write, размер блока и т.п. Лежат в
    NTableClient::TTableReaderConfigPtr TableReader;
    NTableClient::TTableWriterConfigPtr TableWriter;


    //  для журналов (это append-only лог-структура в YT, типа Kafka внутри Cypress). У них своя семантика кворумной записи.
    NApi::TJournalReaderConfigPtr JournalReader;
    NApi::TJournalWriterConfigPtr JournalWriter;

    NChunkClient::TFetcherConfigPtr Fetcher;
    NChunkClient::TChunkFragmentReaderConfigPtr ChunkFragmentReader;
    int ApiVersion;

    i64 ReadBufferRowCount;
    i64 ReadBufferSize;
    i64 WriteBufferSize;

    TSlruCacheConfigPtr ClientCache;

    std::optional<TString> Token;

    //! Target cluster for multiproxy mode.
    std::optional<std::string> MultiproxyTargetCluster;

    TAsyncExpiringCacheConfigPtr ProxyDiscoveryCache;

    NApi::NRpcProxy::EAddressType DefaultRpcProxyAddressType;

    bool EnableInternalCommands;

    bool ExpectStructuredInputInStructuredBatchCommands;

    //! Controls whether authentication commands (SetUserPassword, IssueToken, ListUserTokens, etc.) require a correct password to be used.
    bool RequirePasswordInAuthenticationCommands;

    REGISTER_YSON_STRUCT(TDriverConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TDriverConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDriver
