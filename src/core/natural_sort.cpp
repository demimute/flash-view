#include "viewer/core/natural_sort.h"

#include <cstddef>
#include <cwctype>

namespace viewer::core {
namespace {

[[nodiscard]] constexpr bool is_ascii_digit(wchar_t value) noexcept {
  return value >= L'0' && value <= L'9';
}

[[nodiscard]] std::size_t digit_run_end(std::wstring_view value,
                                        std::size_t begin) noexcept {
  auto end = begin;
  while (end < value.size() && is_ascii_digit(value[end])) {
    ++end;
  }
  return end;
}

[[nodiscard]] std::size_t significant_digit_begin(
    std::wstring_view value, std::size_t begin, std::size_t end) noexcept {
  while (begin < end && value[begin] == L'0') {
    ++begin;
  }
  return begin;
}

}  // namespace

bool NaturalLess::operator()(std::wstring_view lhs,
                             std::wstring_view rhs) const noexcept {
  std::size_t lhs_index = 0;
  std::size_t rhs_index = 0;

  while (lhs_index < lhs.size() && rhs_index < rhs.size()) {
    if (is_ascii_digit(lhs[lhs_index]) && is_ascii_digit(rhs[rhs_index])) {
      const auto lhs_end = digit_run_end(lhs, lhs_index);
      const auto rhs_end = digit_run_end(rhs, rhs_index);
      const auto lhs_significant =
          significant_digit_begin(lhs, lhs_index, lhs_end);
      const auto rhs_significant =
          significant_digit_begin(rhs, rhs_index, rhs_end);
      const auto lhs_significant_length = lhs_end - lhs_significant;
      const auto rhs_significant_length = rhs_end - rhs_significant;

      if (lhs_significant_length != rhs_significant_length) {
        return lhs_significant_length < rhs_significant_length;
      }

      for (std::size_t offset = 0; offset < lhs_significant_length; ++offset) {
        const auto lhs_digit = lhs[lhs_significant + offset];
        const auto rhs_digit = rhs[rhs_significant + offset];
        if (lhs_digit != rhs_digit) {
          return lhs_digit < rhs_digit;
        }
      }

      const auto lhs_run_length = lhs_end - lhs_index;
      const auto rhs_run_length = rhs_end - rhs_index;
      if (lhs_run_length != rhs_run_length) {
        return lhs_run_length < rhs_run_length;
      }

      lhs_index = lhs_end;
      rhs_index = rhs_end;
      continue;
    }

    const auto lhs_folded = std::towlower(lhs[lhs_index]);
    const auto rhs_folded = std::towlower(rhs[rhs_index]);
    if (lhs_folded != rhs_folded) {
      return lhs_folded < rhs_folded;
    }

    ++lhs_index;
    ++rhs_index;
  }

  return lhs_index == lhs.size() && rhs_index != rhs.size();
}

}  // namespace viewer::core
