/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          socket_compat.hpp
        -\-    _|__
         |\___/  . \        Created on 17 Nov. 2026 at 13:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

// Sockets POSIX / Winsock sous les memes noms. Un socket est garde dans un std::intptr_t : -1 si ferme
// (INVALID_SOCKET de Winsock vaut aussi -1 une fois converti).

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifndef MSG_NOSIGNAL
// macOS : pas de MSG_NOSIGNAL, SIGPIPE est coupe par SO_NOSIGPIPE a l'ouverture du socket ; Windows : pas de SIGPIPE
#define MSG_NOSIGNAL 0
#endif

namespace qrprotec {

using socket_handle = std::intptr_t;

#ifdef _WIN32

// WSAStartup une seule fois (jamais de WSACleanup : les sockets vivent jusqu'a la sortie)
inline bool sockets_ready() {
  static const bool ready = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}
inline SOCKET native_socket(socket_handle handle) { return static_cast< SOCKET >(handle); }
inline void   close_socket(socket_handle handle) {
  ::shutdown(native_socket(handle), SD_BOTH);
  ::closesocket(native_socket(handle));
}
inline int socket_poll(pollfd *descriptors, unsigned long count, int timeout_ms) {
  return ::WSAPoll(descriptors, count, timeout_ms);
}
// Delai d'envoi / de reception : un DWORD en millisecondes sous Windows
inline void set_socket_timeouts(socket_handle handle, int timeout_ms) {
  const DWORD value = static_cast< DWORD >(timeout_ms);
  ::setsockopt(native_socket(handle), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast< const char * >(&value), sizeof(value));
  ::setsockopt(native_socket(handle), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast< const char * >(&value), sizeof(value));
}
inline int  last_socket_error() { return ::WSAGetLastError(); }
inline bool socket_timed_out(int code) { return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK; }

#else

inline bool sockets_ready() { return true; }
inline int  native_socket(socket_handle handle) { return static_cast< int >(handle); }
inline void close_socket(socket_handle handle) {
  ::shutdown(native_socket(handle), SHUT_RDWR);
  ::close(native_socket(handle));
}
inline int socket_poll(pollfd *descriptors, unsigned long count, int timeout_ms) {
  return ::poll(descriptors, static_cast< nfds_t >(count), timeout_ms);
}
inline void set_socket_timeouts(socket_handle handle, int timeout_ms) {
  timeval timeout{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
  ::setsockopt(native_socket(handle), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(native_socket(handle), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

#endif

} // namespace qrprotec
