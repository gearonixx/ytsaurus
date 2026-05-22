#pragma once

#include "private.h"

#include <util/generic/string.h>

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

struct TProtocolVersion
{
    int Major;
    int Minor;

    bool operator==(const TProtocolVersion& other) const = default;

    static TProtocolVersion FromString(TStringBuf protocolVersionString);
};

void FormatValue(TStringBuilderBase* builder, TProtocolVersion version, TStringBuf spec);

////////////////////////////////////////////////////////////////////////////////

constexpr TProtocolVersion GenericProtocolVersion{-1, -1};


//    то "нулевая точка отсчёта" — версия для сервисов, которые либо:
//
// Новые и пока не нуждаются в версионировании.
// Старые и никогда не вводили версионирование (legacy).
// Внутренние/служебные, где совместимость не важна.
constexpr TProtocolVersion DefaultProtocolVersion{0, 0};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
