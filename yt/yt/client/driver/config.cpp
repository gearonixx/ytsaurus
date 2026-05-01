#include "config.h"

#include <yt/yt/client/api/config.h>

#include <yt/yt/client/chunk_client/config.h>

#include <yt/yt/client/table_client/config.h>

#include <yt/yt/core/misc/cache_config.h>

namespace NYT::NDriver {

////////////////////////////////////////////////////////////////////////////////
///
///
/// @gearonixx
    /// Parameter — «у меня есть такое поле».
    // Preprocessor — «вот сложные дефолты, поставь их перед чтением файла».
    // Postprocessor — «проверь, что всё это вместе осмысленно после чтения файла».

    // Parameter — объявляет одно поле: «вот моё поле X, в YSON оно называется так-то, дефолт такой-то».
    //
    // Preprocessor — донастраивает вложенные конфиги до чтения файла: «прежде чем читать YSON, поменяй мне вот эти поля во вложенных объектах под мой контекст».
    //
    // Postprocessor — проверяет всё после чтения файла: «когда всё прочитал, убедись, что значения осмысленные и не противоречат друг другу, иначе кинь ошибку».



void TDriverConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("file_reader", &TThis::FileReader)
        .DefaultNew();
    registrar.Parameter("file_writer", &TThis::FileWriter)
        .DefaultNew();
    registrar.Parameter("table_reader", &TThis::TableReader)
        .DefaultNew();
    registrar.Parameter("table_writer", &TThis::TableWriter)
        .DefaultNew();
    registrar.Parameter("journal_reader", &TThis::JournalReader)
        .DefaultNew();
    registrar.Parameter("journal_writer", &TThis::JournalWriter)
        .DefaultNew();
    registrar.Parameter("fetcher", &TThis::Fetcher)
        .DefaultNew();
    registrar.Parameter("chunk_fragment_reader", &TThis::ChunkFragmentReader)
        .DefaultNew();

    registrar.Parameter("read_buffer_row_count", &TThis::ReadBufferRowCount)
        .Default(10'000);
    registrar.Parameter("read_buffer_size", &TThis::ReadBufferSize)
        .Default(1_MB);
    registrar.Parameter("write_buffer_size", &TThis::WriteBufferSize)
        .Default(1_MB);

    registrar.Parameter("client_cache", &TThis::ClientCache)
        .DefaultNew();

    registrar.Parameter("api_version", &TThis::ApiVersion)
        .Default(ApiVersion3)
        .GreaterThanOrEqual(ApiVersion3)
        .LessThanOrEqual(ApiVersion4);

    registrar.Parameter("token", &TThis::Token)
        .Optional();

    registrar.Parameter("multiproxy_target_cluster", &TThis::MultiproxyTargetCluster)
        .Optional();

    registrar.Parameter("proxy_discovery_cache", &TThis::ProxyDiscoveryCache)
        .DefaultNew();

    // @gearonixx @@rpc_proxy
    // Discovery - это процесс, при котором клиент сам узнаёт адреса нужных ему сервисов, а не получает их жёстко прописанными в конфиге.

    //  примерно так: клиент знает только адрес кластера (или мастера), подключается к нему и спрашивает «дай мне список доступных RPC-прокси».
    //  В ответ из Cypress (это древовидное метахранилище YT, что-то вроде распределённой ФС с конфигами) приходит список прокси с их адресами.
    //  Клиент выбирает один и дальше ходит уже через него.

    // , какой тип адреса RPC-прокси клиент берёт по умолчанию из Cypress при дискавери
    // здесь InternalRpc, то есть внутренний адрес кластера, а не внешний/балансерный.
    // чем отличаются внутренний и внешний?
    registrar.Parameter("default_rpc_proxy_address_type", &TThis::DefaultRpcProxyAddressType)
        .Default(NApi::NRpcProxy::EAddressType::InternalRpc);

    registrar.Parameter("enable_internal_commands", &TThis::EnableInternalCommands)
        .Default(false);

    registrar.Parameter("expect_structured_input_in_structured_batch_commands", &TThis::ExpectStructuredInputInStructuredBatchCommands)
        .Default(true);

    registrar.Parameter("require_password_in_authentication_commands", &TThis::RequirePasswordInAuthenticationCommands)
        .Default(true);

    // registrar.Preprocessor([...]) — это «зарегистрировать функцию, которая выполнится после создания объекта,
    // но до парсинга YSON-файла».
    // Сама лямбда [](TThis* config) { ... } — это функция, принимающая указатель на конструируемый объект.


 //    def setup_defaults(config):
 //     config.client_cache.capacity = 1024 * 1024  # 1 МБ в байтах
 //     config.proxy_discovery_cache.refresh_time = 15
 //     config.proxy_discovery_cache.expiration_period = 15
 //     config.proxy_discovery_cache.expire_after_successful_update_time = 15
 //     config.proxy_discovery_cache.expire_after_failed_update_time = 15
 //
 // registrar.add_preprocessor(setup_defaults)

    // выполни эту функцию до парсинга YSON-файла, чтобы программно проставить дефолты во вложенные конфиги
    // й. Preprocessor нужен, чтобы переопределить эти
    // дефолты под конкретный контекст (прокси хочет 15 секунд там, где общий дефолт — 60), не меняя их в самом вложенном классе для всех остальных.


    // это место, где прокси говорит «когда я создаюсь, после общих дефолтов вложенного класса донастрой его поля под меня»,
    registrar.Preprocessor([] (TThis* config) {
        config->ClientCache->Capacity = 1024_KB;
        config->ProxyDiscoveryCache->RefreshTime = TDuration::Seconds(15);
        config->ProxyDiscoveryCache->ExpirationPeriod = TDuration::Seconds(15);
        config->ProxyDiscoveryCache->ExpireAfterSuccessfulUpdateTime = TDuration::Seconds(15);
        config->ProxyDiscoveryCache->ExpireAfterFailedUpdateTime = TDuration::Seconds(15);
    });

    registrar.Postprocessor([] (TThis* config) {
        if (config->ApiVersion != ApiVersion3 && config->ApiVersion != ApiVersion4) {
            THROW_ERROR_EXCEPTION("Unsupported API version %v",
                config->ApiVersion);
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDriver
