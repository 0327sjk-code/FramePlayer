#pragma once

#include "Update/UpdateTypes.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace zt::sequence::updating::detail {

[[nodiscard]] UpdateFailure MakeFailure(
    UpdateErrorCode code,
    std::wstring message,
    std::uint32_t nativeCode = 0,
    std::uint32_t httpStatus = 0);

[[nodiscard]] UpdateFailure MakeNativeFailure(
    UpdateErrorCode code,
    std::wstring_view context,
    std::uint32_t nativeCode);

[[nodiscard]] UpdateFailure MakeCancelledFailure();

}  // namespace zt::sequence::updating::detail
