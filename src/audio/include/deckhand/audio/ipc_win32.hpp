#pragma once
// Windows-only helpers of the engine IPC layer that are not part of the portable OS interface in ipc.hpp.
#if defined(_WIN32)
#include <functional>

namespace deckhand::ipc {

// Runs `on_quit` once, on a system thread, when the process gets Ctrl+C, Ctrl+Break, a console close, a logoff or a
// shutdown (the Windows counterparts of SIGINT / SIGHUP / SIGTERM), and then lets the default handling end the
// process. A process that is ended with TerminateProcess never sees any of these: that is the "crash" case, and the
// engine process keeps playing. nullptr removes the handler and waits for one that is running.
void set_quit_handler(std::function<void()> on_quit);

}  // namespace deckhand::ipc
#endif
