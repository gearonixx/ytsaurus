#pragma once

// @gearonixx
//! Defines a global variable that is initialized on its first access.
/*!
 *  In contrast to a usual variable with static storage duration, this one
 *  is not susceptible to initialization order fiasco issues.
 */
// YT_DEFINE_GLOBAL разворачивается в функцию, которая возвращает ссылку
// на свою static-переменную (lazy-initialized глобал, без static init fiasco):
//
//   inline const NLogging::TLogger& HttpProxyLogger()
//   {
//       static const NLogging::TLogger result{"HttpProxy"};
//       return result;
//   }
//
// Поэтому ниже HttpProxyLogger используется как функция: HttpProxyLogger().
// NLogging == Namespace Logging
#define YT_DEFINE_GLOBAL(type, name, ...) \
    inline type& name() \
    { \
        static type result{__VA_ARGS__}; \
        return result;  \
    }
