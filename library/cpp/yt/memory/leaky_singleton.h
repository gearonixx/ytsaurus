#pragma once

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TLeakyStorage
{
public:
    template <class... TArgs>
    explicit TLeakyStorage(TArgs&&... args);

    T* Get();

private:
    // Это критично: процессоры на многих архитектурах падают или работают медленно при чтении невыровненных данных.
    // — то T бы автоматически создался в конструкторе и автоматически разрушился в деструкторе. А нам нужно ровно наоборот: создать вручную (через placement-new) и никогда не разрушать.
    // На x86 — медленнее работает (процессор делает два чтения вместо одного).
    // На ARM, RISC-V и др. — краш (SIGBUS, hardware exception).
    // Некоторые SIMD-инструкции на x86 тоже крашатся на невыровненных данных.

   //  Чтобы два потока, пишущих в соседние счётчики, не дрались за одну кэш-линию процессора.
    alignas(T) char Buffer_[sizeof(T)];
};

////////////////////////////////////////////////////////////////////////////////

#define DECLARE_LEAKY_SINGLETON_FRIEND() \
    template <class T>                   \
    friend class ::NYT::TLeakyStorage;

template <class T, class... TArgs>
T* LeakySingleton(TArgs&&... args);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define LEAKY_SINGLETON_INL_H_
#include "leaky_singleton-inl.h"
#undef LEAKY_SINGLETON_INL_H_
