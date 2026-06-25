#pragma once

#include <string_view>

namespace viewer::core {

struct NaturalLess {
  [[nodiscard]] bool operator()(std::wstring_view lhs,
                                std::wstring_view rhs) const noexcept;
};

}  // namespace viewer::core
