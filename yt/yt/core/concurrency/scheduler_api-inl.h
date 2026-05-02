#ifndef SCHEDULER_API_INL_H_
#error "Direct inclusion of this file is not allowed, include scheduler_api.h"
// For the sake of sane code completion.
#include "scheduler_api.h"
#endif
#undef SCHEDULER_API_INL_H_

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////
// NB: Please refer to YT-18899 before trying to pass future to
// these functions by const-ref.

template <CFuture TFuture>
TErrorOr<typename TFuture::TValueType> WaitFor(TFuture future, IInvokerPtr invoker)
{
    YT_ASSERT(future);
    YT_ASSERT(invoker);

    // @gearonixx @@concurrency
    // wait_until_set(future.as_void(), invoker)  # уступает файбер пока future не готов
    // # засыпаем пока future не готов, OS-тред в это время делает другую работу

    // Конвертирует TFuture<T> в TFuture<void>

    // WaitUntilSet принимает только TFuture<void>,
    // потому что ему плевать на тип результата, ему нужно только "когда"
    WaitUntilSet(future.AsVoid(), std::move(invoker));

    //  # TErrorOr<T>: либо значение, либо ошибка
    //  return future.result()
    return future.GetOrCrash();
}

template <CFuture TFuture>
TErrorOr<typename TFuture::TValueType> WaitForFast(TFuture future)
{
    YT_ASSERT(future);
    YT_ASSERT(!IsContextSwitchForbidden());

    if (!future.IsSet()) {
        WaitUntilSet(future.AsVoid(), GetCurrentInvoker());
    }

    return future.GetOrCrash();
}

template <CFuture TFuture>
TErrorOr<typename TFuture::TValueType> WaitForWithStrategy(TFuture future, EWaitForStrategy strategy)
{
    switch (strategy) {
        case EWaitForStrategy::WaitFor:
            return WaitFor(future);
        case EWaitForStrategy::Get:
            return future.BlockingGet();
        default:
            YT_ABORT();
    }
}

inline void Yield()
{
    WaitUntilSet(OKFuture);
}

inline void SwitchTo(IInvokerPtr invoker)
{
    WaitUntilSet(OKFuture, std::move(invoker));
}

////////////////////////////////////////////////////////////////////////////////

} //namespace NYT::NConcurrency
