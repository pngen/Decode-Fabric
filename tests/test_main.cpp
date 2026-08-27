#include "test_framework.hpp"
#include <cstdio>
int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  return df_test::run_all(argc, argv);
}
