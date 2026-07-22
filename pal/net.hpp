// Implements 019-Platform-Abstraction-Layer §2 (async I/O event loop + sockets) — the OS-selector
// entry point. include/quark/net/tcp_transport.hpp includes ONLY this header, never a per-OS one
// directly and never `<sys/*>`/`<netinet/*>`/`<winsock2.h>` (019 §"The one rule"). See
// pal/linux_x86_64/net.hpp / pal/windows_x86_64/net.hpp for the concrete surface (fd_t, invalid_fd,
// IoContext, EPOLLIN et al., tcp_listen/tcp_connect/accept_one/recv_some/send_some/...) — identical
// shape on both backends.
#pragma once

#if defined(_WIN32)
#include "pal/windows_x86_64/net.hpp"
#else
#include "pal/linux_x86_64/net.hpp"
#endif
