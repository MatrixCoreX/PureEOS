// Test-only syscall wrappers: never changes the user's real terminal.
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <system_error>

namespace fc { void set_console_echo(bool); }
namespace {
struct observation {
   bool tty = true;
   int get_result = 0, set_result = 0;
   unsigned get_calls = 0, set_calls = 0;
   termios initial{}, applied{};
} state;
}
extern "C" int __wrap_isatty(int fd) { return fd == STDIN_FILENO && state.tty; }
extern "C" int __wrap_tcgetattr(int, termios* value) {
   ++state.get_calls;
   // Also initialize on injected failure: demonstrate the old control-flow
   // bug WITHOUT deliberately executing its real uninitialized-memory read.
   std::memcpy(value, &state.initial, sizeof(*value));
   errno = EIO;
   return state.get_result;
}
extern "C" int __wrap_tcsetattr(int, int, const termios* value) {
   ++state.set_calls;
   std::memcpy(&state.applied, value, sizeof(*value));
   errno = EIO;
   return state.set_result;
}
int main() {
   unsigned cases = 0, passed = 0;
   for (bool enable : {false, true}) for (unsigned mode = 0; mode < 4; ++mode) {
      state = {};
      state.initial.c_lflag = ECHO | ICANON;
      state.tty = mode != 0;
      if (mode == 0 || mode == 1) state.get_result = -1;
      if (mode == 2) state.set_result = -1;
      bool threw = false, right_error = true;
      try { fc::set_console_echo(enable); }
      catch (const std::system_error& error) { threw = true; right_error = error.code().value() == EIO; }
      catch (...) { threw = true; right_error = false; }
      bool good;
      if (mode == 0) good = !threw && state.get_calls == 0 && state.set_calls == 0;
      else if (mode == 1) good = threw && right_error && state.get_calls == 1 && state.set_calls == 0;
      else if (mode == 2) good = threw && right_error && state.get_calls == 1 && state.set_calls == 1;
      else good = !threw && state.get_calls == 1 && state.set_calls == 1 &&
         state.applied.c_lflag == (enable ? (ECHO | ICANON) : ICANON);
      ++cases; passed += good;
      std::cout << "case mode=" << mode << " echo=" << enable << " passed=" << good << '\n';
   }
   std::cout << "cases=" << cases << " passed=" << passed << '\n';
   return cases == passed ? 0 : 1;
}
