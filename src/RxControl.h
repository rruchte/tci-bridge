#pragma once

#include <QMetaType>

#include <array>
#include <optional>

enum class RxControl { RitEnabled, RitOffset, XitEnabled, XitOffset, NoiseBlanker, NoiseReduction, Count };

inline constexpr auto rxControlCount = static_cast<std::size_t>(RxControl::Count);
using RxControlValues = std::array<std::optional<int>, rxControlCount>;

inline constexpr bool rxControlIsSwitch(RxControl setting)
{
	return setting != RxControl::RitOffset && setting != RxControl::XitOffset;
}

Q_DECLARE_METATYPE(RxControl)
