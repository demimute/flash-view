#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <clocale>
#include <string>
#include <string_view>

#include "viewer/core/natural_sort.h"

namespace viewer::core {
namespace {

class CTypeLocaleGuard {
 public:
  CTypeLocaleGuard() : original_(std::setlocale(LC_CTYPE, nullptr)) {}

  CTypeLocaleGuard(const CTypeLocaleGuard&) = delete;
  CTypeLocaleGuard& operator=(const CTypeLocaleGuard&) = delete;

  ~CTypeLocaleGuard() { std::setlocale(LC_CTYPE, original_.c_str()); }

 private:
  std::string original_;
};

static_assert(noexcept(NaturalLess{}(std::wstring_view{},
                                     std::wstring_view{})));

TEST(NaturalSortTest, SortsDigitRunsByNumericValue) {
  std::array values{std::wstring_view{L"10"}, std::wstring_view{L"2"},
                    std::wstring_view{L"1"}};

  std::sort(values.begin(), values.end(), NaturalLess{});

  EXPECT_EQ(values, (std::array{std::wstring_view{L"1"},
                                std::wstring_view{L"2"},
                                std::wstring_view{L"10"}}));
}

TEST(NaturalSortTest, TreatsCaseVariantsAsEquivalent) {
  const NaturalLess less;

  EXPECT_FALSE(less(L"A", L"a"));
  EXPECT_FALSE(less(L"a", L"A"));
}

TEST(NaturalSortTest, UsesDigitRunLengthToBreakNumericTies) {
  const NaturalLess less;

  EXPECT_TRUE(less(L"2", L"002"));
  EXPECT_FALSE(less(L"002", L"2"));
}

TEST(NaturalSortTest, OrdersZeroValuedRunsByRunLength) {
  const NaturalLess less;

  EXPECT_TRUE(less(L"file0", L"file00"));
  EXPECT_TRUE(less(L"file00", L"file000"));
}

TEST(NaturalSortTest, ComparesVeryLongDigitRunsWithoutIntegerConversion) {
  constexpr std::wstring_view smaller =
      L"9999999999999999999999999999999999999999";
  constexpr std::wstring_view larger =
      L"10000000000000000000000000000000000000000";
  const NaturalLess less;

  EXPECT_TRUE(less(smaller, larger));
  EXPECT_FALSE(less(larger, smaller));
}

TEST(NaturalSortTest, OrdersPrefixesBeforeLongerStrings) {
  const NaturalLess less;

  EXPECT_TRUE(less(L"image", L"image2"));
  EXPECT_FALSE(less(L"image2", L"image"));
}

TEST(NaturalSortTest, FoldsAsciiCaseAfterChineseCodeUnits) {
  const NaturalLess less;

  EXPECT_FALSE(less(L"\u56FEA", L"\u56FEa"));
  EXPECT_FALSE(less(L"\u56FEa", L"\u56FEA"));
  EXPECT_TRUE(less(L"\u56FEa", L"\u56FEx"));
}

TEST(NaturalSortTest, OrdersNonAsciiCodeUnitsByRawValue) {
  const NaturalLess less;

  EXPECT_TRUE(less(L"\u00C4", L"\u00E4"));
  EXPECT_FALSE(less(L"\u00E4", L"\u00C4"));
}

TEST(NaturalSortTest, IgnoresGlobalLocaleWhenComparingNonAsciiCodeUnits) {
  CTypeLocaleGuard locale_guard;
  const NaturalLess less;

  ASSERT_NE(std::setlocale(LC_CTYPE, "C"), nullptr);
  const bool c_forward = less(L"\u00C4", L"\u00E4");
  const bool c_reverse = less(L"\u00E4", L"\u00C4");

  if (std::setlocale(LC_CTYPE, "en_US.UTF-8") == nullptr) {
    GTEST_SKIP() << "en_US.UTF-8 locale is unavailable";
  }

  EXPECT_EQ(less(L"\u00C4", L"\u00E4"), c_forward);
  EXPECT_EQ(less(L"\u00E4", L"\u00C4"), c_reverse);
}

TEST(NaturalSortTest, CaseInsensitiveEquivalenceIsTransitive) {
  const NaturalLess less;
  const auto equivalent = [&less](std::wstring_view lhs,
                                  std::wstring_view rhs) {
    return !less(lhs, rhs) && !less(rhs, lhs);
  };

  EXPECT_TRUE(equivalent(L"FILE2", L"File2"));
  EXPECT_TRUE(equivalent(L"File2", L"file2"));
  EXPECT_TRUE(equivalent(L"FILE2", L"file2"));
}

TEST(NaturalSortTest, FormsStrictWeakOrderingAcrossRepresentativeValues) {
  constexpr std::array values{
      std::wstring_view{L""},       std::wstring_view{L"a"},
      std::wstring_view{L"A"},      std::wstring_view{L"a0"},
      std::wstring_view{L"a00"},    std::wstring_view{L"a2"},
      std::wstring_view{L"a002"},   std::wstring_view{L"a10"},
      std::wstring_view{L"\u00C4"}, std::wstring_view{L"\u00E4"},
      std::wstring_view{L"\u56FEa"}, std::wstring_view{L"\u56FEx"},
  };
  const NaturalLess less;

  for (const auto lhs : values) {
    EXPECT_FALSE(less(lhs, lhs));
    for (const auto rhs : values) {
      if (less(lhs, rhs)) {
        EXPECT_FALSE(less(rhs, lhs));
      }
      for (const auto third : values) {
        if (less(lhs, rhs) && less(rhs, third)) {
          EXPECT_TRUE(less(lhs, third));
        }
      }
    }
  }
}

}  // namespace
}  // namespace viewer::core
