#include <gtest/gtest.h>

#include "doctor.hpp"

TEST(Doctor, EveryRequiredCheckPasses) {
  for (const auto& c : cutoff::run_doctor_checks()) {
    if (c.required) EXPECT_TRUE(c.ok) << c.name << ": " << c.detail;
    EXPECT_FALSE(c.detail.empty()) << c.name;
  }
}
