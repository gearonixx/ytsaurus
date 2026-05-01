#pragma once

#include "public.h"

#include <yt/yt/core/logging/log.h>

#include <yt/yt/library/profiling/sensor.h>

namespace NYT::NHttpProxy {

////////////////////////////////////////////////////////////////////////////////

constexpr auto HttpProxyUserAllocationTagKey = "user";
constexpr auto HttpProxyRequestIdAllocationTagKey = "request_id";
constexpr auto HttpProxyCommandAllocationTagKey = "command";

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, HttpProxyLogger, "HttpProxy");
    // @gearonixx
    // Профайлер пишет метрики — числа, которые меняются во времени — для систем мониторинга, которые рисуют графики.
    // см. Мониторинг -> Профайлер в excalidraw

// Что попадает в профайлер: «в момент T значение метрики /http_proxy/request_count = 1234». Это потом скрейпится Прометеем, Grafana, Solomon — и рисуются графики.
//     /http_proxy/active_connections — сколько коннектов открыто прямо сейчас
// /http_proxy/queue_size — сколько запросов в очереди на обработку
// /http_proxy/memory_usage — сколько памяти жрёт процесс
    // Хранится в отдельных переменных в памяти процесса, которые код инкрементирует/обновляет вручную в нужных местах. С HTTP-запросами напрямую они не связаны — это статистика про запросы, не из запросов.
YT_DEFINE_GLOBAL(const NProfiling::TProfiler, HttpProxyProfiler, "/http_proxy");

extern const NLogging::TLogger HttpStructuredProxyLogger;

DECLARE_REFCOUNTED_CLASS(TFramingAsyncOutputStream)

DECLARE_REFCOUNTED_STRUCT(TApiTestingOptions)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
