#include <iostream>
#include <string>
#include <cerrno>
#include <system_error>

namespace fc {

#ifdef WIN32
#include <windows.h>

void set_console_echo( bool enable_echo )
{
   auto stdin_handle = GetStdHandle( STD_INPUT_HANDLE );
   DWORD mode = 0;
   GetConsoleMode( stdin_handle, &mode );
   if( enable_echo )
   {
      SetConsoleMode( stdin_handle, mode | ENABLE_ECHO_INPUT );
   }
   else
   {
      SetConsoleMode( stdin_handle, mode & (~ENABLE_ECHO_INPUT) );
   }
}

#else // NOT WIN32
#include <termios.h>
#include <unistd.h>

void set_console_echo( bool enable_echo )
{
   // A pipe/file has no terminal echo to change. In particular, tcgetattr
   // fails there and must never leave an uninitialized termios in use.
   if( !isatty(STDIN_FILENO) ) return;
   termios oldt{};
   if( tcgetattr(STDIN_FILENO, &oldt) != 0 )
      throw std::system_error(errno, std::generic_category(), "cannot read terminal echo state");
   termios newt = oldt;
   if( enable_echo )
   {
      newt.c_lflag |= ECHO;
   }
   else
   {
      newt.c_lflag &= ~ECHO;
   }
   if( tcsetattr(STDIN_FILENO, TCSANOW, &newt) != 0 )
      throw std::system_error(errno, std::generic_category(), "cannot change terminal echo state");
}

#endif // WIN32

} // namespace fc
