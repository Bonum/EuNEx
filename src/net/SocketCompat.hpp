#pragma once
// Cross-platform socket abstraction for FIXGatewayActor

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>

  using socket_t = SOCKET;
  constexpr socket_t INVALID_SOCK = INVALID_SOCKET;

  inline int closeSocket(socket_t s) { return closesocket(s); }
  inline int socketError() { return WSAGetLastError(); }

  struct SocketInit {
      SocketInit() { WSADATA w; WSAStartup(MAKEWORD(2,2), &w); }
      ~SocketInit() { WSACleanup(); }
  };

#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <cerrno>
  #include <fcntl.h>

  using socket_t = int;
  constexpr socket_t INVALID_SOCK = -1;

  inline int closeSocket(socket_t s) { return close(s); }
  inline int socketError() { return errno; }

  struct SocketInit {
      SocketInit() {}
      ~SocketInit() {}
  };

#endif

inline bool setSocketNonBlocking(socket_t s) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

inline bool setSocketReuseAddr(socket_t s) {
    int opt = 1;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
}
