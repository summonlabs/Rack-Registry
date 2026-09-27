// Rack Registry - minimal test framework implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"

#include <iostream>

namespace rrtest {
namespace {

TestCase* current_test = nullptr;
int current_failures = 0;

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

bool register_test(std::string name, TestFunction function) {
  registry().push_back(TestCase{std::move(name), function});
  return true;
}

void report_failure(const char* file, int line, const std::string& message) {
  ++current_failures;
  std::cout << "  FAIL " << file << ":" << line << "\n    " << message << "\n";
  std::cout.flush();
}

int run_all(std::string_view filter) {
  int failed_tests = 0;
  int ran_tests = 0;
  for (const TestCase& test : registry()) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    ++ran_tests;
    current_test = const_cast<TestCase*>(&test);
    current_failures = 0;
    std::cout << "[ RUN  ] " << test.name << "\n";
    std::cout.flush();
    test.function();
    if (current_failures == 0) {
      std::cout << "[  OK  ] " << test.name << "\n";
    } else {
      std::cout << "[ FAIL ] " << test.name << " (" << current_failures << " failures)\n";
      ++failed_tests;
    }
    std::cout.flush();
  }
  (void)current_test;
  std::cout << (failed_tests == 0 ? "PASSED " : "FAILED ") << (ran_tests - failed_tests) << "/"
            << ran_tests << " tests\n";
  return failed_tests;
}

}  // namespace rrtest

int main(int argc, char** argv) {
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    if (token == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (token == "--list") {
      for (const rrtest::TestCase& test : rrtest::registry()) {
        std::cout << test.name << "\n";
      }
      return 0;
    }
  }
  return rrtest::run_all(filter) == 0 ? 0 : 1;
}
