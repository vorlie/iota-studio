#include "editor_app.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  bool smokeTest = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--smoke-test") {
      smokeTest = true;
    }
  }

  iota::studio::EditorApp app;
  if (!app.initialize(smokeTest)) {
    return 1;
  }

  const int rc = smokeTest ? app.runSmokeTest() : app.run();
  app.shutdown();
  return rc;
}
