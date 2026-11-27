// Copyright (C) 2026 Vorlie
// This file is part of Iota Studio.
//
// Iota Studio is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Iota Studio is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
// See the LICENSE file for the full license text.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "editor_app.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <string_view>

int main(int argc, char** argv) {
  bool smokeTest = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--smoke-test") {
      smokeTest = true;
    }
  }

#ifdef __EMSCRIPTEN__
  // Heap-allocate under Emscripten so emscripten_set_main_loop_arg unwinding never destroys app state.
  auto* app = new iota::studio::EditorApp();
  if (!app->initialize(false)) {
    delete app;
    return 1;
  }
  return app->run();
#else
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
#endif
}
