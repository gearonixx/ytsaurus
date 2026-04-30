#include "shutdown.h"

#include <yt/yt/core/concurrency/system_invokers.h>

#include <yt/yt/core/misc/collection_helpers.h>
#include <yt/yt/core/misc/proc.h>

#include <library/cpp/yt/cpu_clock/clock.h>

#include <library/cpp/yt/threading/fork_aware_spin_lock.h>
#include <library/cpp/yt/threading/event_count.h>

#include <library/cpp/yt/misc/tls.h>

#include <library/cpp/yt/system/exit.h>

#include <library/cpp/yt/memory/leaky_singleton.h>

#include <util/generic/algorithm.h>

#include <util/system/env.h>
#include <util/system/thread.h>

#include <thread>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////
///
///
// Так что HungExitCode = «код возврата на случай зависания»

class TShutdownManager
{
public:
    // Это паттерн Singleton — гарантия, что у TShutdownManager существует ровно один экземпляр на всю программу, доступный из любого места через TShutdownManager::Get().
    static TShutdownManager* Get()
    {
        //  один экземпляр, разрушается при выходе из программы.
        // Leaky Singleton — один экземпляр, не разрушается никогда (ОС сама заберёт память при убийстве процесса).
        // чтобы объект был жив до самого конца, даже когда другие глобальные объекты в своих деструкторах его дёргают. Иначе словишь use-after-free из-за непредсказуемого порядка разрушения статиков._оj_j
        return LeakySingleton<TShutdownManager>();
    }

    TShutdownCookie RegisterShutdownCallback(
        std::string name,
        TClosure callback,
        int priority)
    {
        auto guard = Guard(Lock_);

        if (ShutdownStarted_.load()) {
            if (auto* logFile = TryGetShutdownLogFile()) {
                ::fprintf(logFile, "%s\t*** Attempt to register shutdown callback when shutdown is already in progress (Name: %s)\n",
                    GetInstant().ToString().c_str(),
                    name.c_str());
            }
            return nullptr;
        }

        auto registeredCallback = New<TRefCountedRegisteredCallback>();
        registeredCallback->Name = std::move(name);
        registeredCallback->Callback = std::move(callback);
        registeredCallback->Priority = priority;
        InsertOrCrash(RegisteredCallbacks_, registeredCallback.Get());

        if (auto* logFile = TryGetShutdownLogFile()) {
            ::fprintf(logFile, "%s\t*** Shutdown callback registered (Name: %s, Priority: %d)\n",
                GetInstant().ToString().c_str(),
                registeredCallback->Name.c_str(),
                registeredCallback->Priority);
        }

        return registeredCallback;
    }

    // @gearonixx @core1
    // the core shutdown function
    void Shutdown(const TShutdownOptions& options = {})
    {
        std::vector<TRegisteredCallback> registeredCallbacks;

        {
            // doing the guard lock
            auto guard = Guard(Lock_);

            // ok it's if true end
            if (ShutdownStarted_.load()) {
                return;
            }

            // if not started start
            ShutdownStarted_.store(true);
            // yup
            ShutdownThreadId_.store(GetCurrentThreadId());

            if (auto* logFile = TryGetShutdownLogFile()) {
                ::fprintf(logFile, "%s\t*** Shutdown started (ThreadId: %" PRISZT ")\n",
                    GetInstant().ToString().c_str(),
                    GetCurrentThreadId());
            }

            for (auto* registeredCallback : RegisteredCallbacks_) {
                registeredCallbacks.push_back(*registeredCallback);
            }
        }

        SortBy(registeredCallbacks, [] (const auto& registeredCallback) {
            return registeredCallback.Priority;
        });

    // Starting threads in exit handlers on Windows causes immediate calling exit
    // so the routine will not be executed. Moreover, if we try to join this thread we'll get deadlock
    // because this thread will try to acquire atexit lock which is owned by this thread
    #ifndef _win_
        NThreading::TEvent shutdownCompleteEvent;
        std::thread watchdogThread([&] {
            ::TThread::SetCurrentThreadName("ShutdownWD");
            if (!shutdownCompleteEvent.Wait(options.GraceTimeout)) {
                if (options.AbortOnHang) {
                    YT_ABORT();
                } else {
                    AbortProcessDramatically(
                        options.HungExitCode,
                        /*exitCodeStr*/ TStringBuf(),
                        "Shutdown hung");
                }
            }
        });
    #endif

        for (auto it = registeredCallbacks.rbegin(); it != registeredCallbacks.rend(); it++) {
            const auto& registeredCallback = *it;
            if (auto* logFile = TryGetShutdownLogFile()) {
                ::fprintf(logFile, "%s\t*** Running callback (Name: %s, Priority: %d)\n",
                    GetInstant().ToString().c_str(),
                    registeredCallback.Name.c_str(),
                    registeredCallback.Priority);
            }
            registeredCallback.Callback();
        }

    #ifndef _win_
        shutdownCompleteEvent.NotifyOne();
        watchdogThread.join();
    #endif

        if (auto* logFile = TryGetShutdownLogFile()) {
            ::fprintf(logFile, "%s\t*** Shutdown completed\n",
                GetInstant().ToString().c_str());
        }
    }

    void AutoShutdown()
    {
        if (AutoShutdownEnabled_.load()) {
            Shutdown();
        }
    }

    bool IsShutdownStarted()
    {
        return ShutdownStarted_.load();
    }

    void SetAutoShutdownEnabled(bool enabled)
    {
        AutoShutdownEnabled_.store(enabled);
    }

    void EnableShutdownLoggingToStderr()
    {
        ShutdownLogFile_.store(stderr);
    }

    void EnableShutdownLoggingToFile(const std::string& fileName)
    {
        auto* file = fopen(fileName.c_str(), "w");
        if (!file) {
            ::fprintf(stderr, "*** Could not open the shutdown logging file\n");
            return;
        }
        // Although POSIX guarantees fprintf always to be thread-safe (see fprintf(2)),
        // it seems to be a good idea to disable buffering for the log file.
        ::setvbuf(file, nullptr, _IONBF, 0);
        ShutdownLogFile_.store(file);
    }

    FILE* TryGetShutdownLogFile()
    {
        return ShutdownLogFile_.load();
    }

    size_t GetShutdownThreadId()
    {
        return ShutdownThreadId_.load();
    }

    void EnsureSafeShutdown() const
    {
        NConcurrency::GetFinalizerInvoker();
        NConcurrency::GetShutdownInvoker();
    }

private:
    std::atomic<FILE*> ShutdownLogFile_ = IsShutdownLoggingEnabledImpl() ? stderr : nullptr;

    // Это «крутящийся» лок. Когда поток не может его захватить, он не спит, а в цикле проверяет: «свободен? свободен? свободен?» — пока другой поток его не отпустит.
    // «Fork-aware» = «знает про fork()».
    YT_DECLARE_SPIN_LOCK(NThreading::TForkAwareSpinLock, Lock_);

    struct TRegisteredCallback
    {
        std::string Name;
        TClosure Callback;
        int Priority;
    };

    // oh there is the ref counting logic on top
    struct TRefCountedRegisteredCallback
        : public TRegisteredCallback
     // @gearonixx
    // 1. Менеджер shutdown'а
    // 2. Тот, кто зарегистрировал коллбэк
    // те. несколько овнеров -> ref counted
        , public TRefCounted
    {
        ~TRefCountedRegisteredCallback()
        {
            TShutdownManager::Get()->UnregisterShutdownCallback(this);
        }
    };

    // @gearonixx
    // Это реестр коллбэков, которые нужно вызвать при завершении
    // Дубликаты не нужны — если кто-то регистрирует один и тот же коллбэк дважды, второй раз просто ничего не произойдёт.
    std::unordered_set<TRefCountedRegisteredCallback*> RegisteredCallbacks_;
    std::atomic<bool> ShutdownStarted_ = false;
    // @gearonixx
    // AutoShutdownEnabled дословно — «автоматический shutdown включён». То есть флаг, который говорит «можно ли запускать процедуру завершения автоматически».

    //  AutoShutdown уважает рубильник, Shutdown — нет.
    // Shutdown зовут руками (из main, из тестов), AutoShutdown зовут автоматически
    std::atomic<bool> AutoShutdownEnabled_ = true;
    std::atomic<size_t> ShutdownThreadId_ = 0;


    static bool IsShutdownLoggingEnabledImpl()
    {
        auto value = GetEnv("YT_ENABLE_SHUTDOWN_LOGGING");
        value.to_lower();
        return value == "1" || value == "true";
    }

    // @gearonixx
    // why guard here?
    void UnregisterShutdownCallback(TRefCountedRegisteredCallback* registeredCallback)
    {
        // Если два потока одновременно полезут в unordered_set без синхронизации — undefined behavior: повреждение внутренних структур, краш, либо тихая порча данных.
        auto guard = Guard(Lock_);

        // Чтения с разных потоков одновременно — безопасно.
        // Хотя бы одна запись параллельно с чем угодно — UB.
        if (auto* logFile = TryGetShutdownLogFile()) {
            ::fprintf(logFile, "%s\t*** Shutdown callback unregistered (Name: %s, Priority: %d)\n",
                GetInstant().ToString().c_str(),
                registeredCallback->Name.c_str(),
                registeredCallback->Priority);
        }
        // вот здесь
        // а если там было чтение, тогда не нужон было бы делать guard lock
        //

        // Все потоки только читают → guard не нужен, читать одновременно безопасно.
        // Хоть один поток где-то пишет → guard нужен всем, и читателям тоже.

        // То есть она зовёт RegisteredCallbacks_.erase(registeredCallback), и если элемент там не нашёлся — крашит программу ш
        // (потому что это означает рассинхрон: пытаемся отписать то, что не было зарегистрировано).
        EraseOrCrash(RegisteredCallbacks_, registeredCallback);
    }

    DECLARE_LEAKY_SINGLETON_FRIEND()
};

////////////////////////////////////////////////////////////////////////////////

TShutdownCookie RegisterShutdownCallback(
    std::string name,
    TClosure callback,
    int priority)
{
    return TShutdownManager::Get()->RegisterShutdownCallback(
        std::move(name),
        std::move(callback),
        priority);
}

void Shutdown(const TShutdownOptions& options)
{
    TShutdownManager::Get()->Shutdown(options);
}

bool IsShutdownStarted()
{
    return TShutdownManager::Get()->IsShutdownStarted();
}

void SetAutoShutdownEnabled(bool enabled)
{
    TShutdownManager::Get()->SetAutoShutdownEnabled(enabled);
}

void EnableShutdownLoggingToStderr()
{
    TShutdownManager::Get()->EnableShutdownLoggingToStderr();
}

void EnableShutdownLoggingToFile(const std::string& fileName)
{
    TShutdownManager::Get()->EnableShutdownLoggingToFile(fileName);
}

FILE* TryGetShutdownLogFile()
{
    return TShutdownManager::Get()->TryGetShutdownLogFile();
}

size_t GetShutdownThreadId()
{
    return TShutdownManager::Get()->GetShutdownThreadId();
}

void EnsureSafeShutdown()
{
    TShutdownManager::Get()->EnsureSafeShutdown();
}

////////////////////////////////////////////////////////////////////////////////

static const void* ShutdownGuardInitializer = [] {
    class TShutdownGuard
    {
    public:
        ~TShutdownGuard()
        {
            if (auto* logFile = TShutdownManager::Get()->TryGetShutdownLogFile()) {
                fprintf(logFile, "*** Shutdown guard destructed\n");
            }
            TShutdownManager::Get()->AutoShutdown();
        }
    };

    static thread_local TShutdownGuard Guard;
    return nullptr;
}();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
