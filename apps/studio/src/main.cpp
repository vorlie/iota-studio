#include "editor_app.hpp"

#include <exception>
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
  try {
    if (!app.initialize(smokeTest)) {
      return 1;
    }

    const int rc = smokeTest ? app.runSmokeTest() : app.run();
    app.shutdown();
    return rc;
  } catch (const std::exception& ex) {
    std::cerr << "Fatal exception: " << ex.what() << "\n";
    app.shutdown();
    return 1;
  } catch (...) {
    std::cerr << "Fatal unknown exception\n";
    app.shutdown();
    return 1;
  }
}
