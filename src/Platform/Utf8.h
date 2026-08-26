#pragma once

#include <string>
#include <string_view>

namespace zt::sequence {

[[nodiscard]] std::string WideToUtf8(std::wstring_view value);
[[nodiscard]] std::wstring Utf8ToWide(std::string_view value);

}  // namespace zt::sequence
