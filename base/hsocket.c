#include "hsocket.h"

const char* socket_strerror(int err) {
#ifdef OS_WIN
    static char buffer[128];

    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS |
        FORMAT_MESSAGE_MAX_WIDTH_MASK,
        0, err, 0, buffer, sizeof(buffer), NULL);

    return buffer;
#else
    return strerror(err);
#endif
}

int Resolver(const char* host, sockaddr_u* addr) {
    if (inet_pton(AF_INET, host, &addr->sin.sin_addr) == 1) {
        addr->sa.sa_family = AF_INET; // host is ipv4, so easy ;)
        return 0;
    }

#ifdef ENABLE_IPV6
    if (inet_pton(AF_INET6, host, &addr->sin6.sin6_addr) == 1) {
        addr->sa.sa_family = AF_INET6; // host is ipv6
        return 0;
    }
    struct addrinfo* ais = NULL;
    struct addrinfo hint;
    hint.ai_flags = 0;
    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = 0;
    hint.ai_protocol = 0;
    int ret = getaddrinfo(host, NULL, NULL, &ais);
    if (ret != 0 || ais == NULL || ais->ai_addrlen == 0 || ais->ai_addr == NULL) {
        printd("unknown host: %s err:%d:%s\n", host, ret, gai_strerror(ret));
        return ret;
    }
    memcpy(addr, ais->ai_addr, ais->ai_addrlen);
    freeaddrinfo(ais);
#else
    struct hostent* phe = gethostbyname(host);
    if (phe == NULL) {
        printd("unknown host %s err:%d:%s\n", host, h_errno, hstrerror(h_errno));
        return -h_errno;
    }
    addr->sin.sin_family = AF_INET;
    memcpy(&addr->sin.sin_addr, phe->h_addr_list[0], phe->h_length);
#endif
    return 0;
}

int Bind(int port, const char* host, int type) {
#ifdef OS_WIN
    static int s_wsa_initialized = 0;
    if (s_wsa_initialized == 0) {
        s_wsa_initialized = 1;
        WSADATA wsadata;
        WSAStartup(MAKEWORD(2,2), &wsadata);
    }
#endif
    sockaddr_u localaddr;
    socklen_t addrlen = sizeof(localaddr);
    memset(&localaddr, 0, addrlen);
    int ret = sockaddr_assign(&localaddr, host, port);
    if (ret != 0) {
        printf("unknown host: %s\n", host);
        return ret;
    }

    // socket -> setsockopt -> bind
    int sockfd = socket(localaddr.sa.sa_family, type, 0);
    if (sockfd < 0) {
        perror("socket");
        return -socket_errno();
    }

    // NOTE: SO_REUSEADDR means that you can reuse sockaddr of TIME_WAIT status
    int reuseaddr = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseaddr, sizeof(int)) < 0) {
        perror("setsockopt");
        goto error;
    }

    if (bind(sockfd, &localaddr.sa, sockaddrlen(&localaddr)) < 0) {
        perror("bind");
        goto error;
    }

    return sockfd;
error:
    closesocket(sockfd);
    return socket_errno() > 0 ? -socket_errno() : -1;
}

int Listen(int port, const char* host) {
    int sockfd = Bind(port, host, SOCK_STREAM);
    if (sockfd < 0) return sockfd;
    if (listen(sockfd, SOMAXCONN) < 0) {
        perror("listen");
        goto error;
    }
    return sockfd;
error:
    closesocket(sockfd);
    return socket_errno() > 0 ? -socket_errno() : -1;
}

int Connect(const char* host, int port, int nonblock) {
    // Resolver -> socket -> nonblocking -> connect
    sockaddr_u peeraddr;
    socklen_t addrlen = sizeof(peeraddr);
    memset(&peeraddr, 0, addrlen);
    int ret = sockaddr_assign(&peeraddr, host, port);
    if (ret != 0) {
        //printf("unknown host: %s\n", host);
        return ret;
    }
    int connfd = socket(peeraddr.sa.sa_family, SOCK_STREAM, 0);
    if (connfd < 0) {
        perror("socket");
        return -socket_errno();
    }
    if (nonblock) {
        nonblocking(connfd);
    }
    ret = connect(connfd, &peeraddr.sa, sockaddrlen(&peeraddr));
#ifdef OS_WIN
    if (ret < 0 && socket_errno() != WSAEWOULDBLOCK) {
#else
    if (ret < 0 && socket_errno() != EINPROGRESS) {
#endif
        perror("connect");
        goto error;
    }
    return connfd;
error:
    closesocket(connfd);
    return socket_errno() > 0 ? -socket_errno() : -1;
}

int ConnectNonblock(const char* host, int port) {
    return Connect(host, port, 1);
}

int ConnectTimeout(const char* host, int port, int ms) {
    int connfd = Connect(host, port, 1);
    if (connfd < 0) return connfd;
    int err;
    socklen_t optlen = sizeof(err);
    struct timeval tv = {ms/1000, (ms%1000)*1000};
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(connfd, &writefds);
    int ret = select(connfd+1, 0, &writefds, 0, &tv);
    if (ret < 0) {
        perror("select");
        goto error;
    }
    if (ret == 0) {
        errno = ETIMEDOUT;
        goto error;
    }
    if (getsockopt(connfd, SOL_SOCKET, SO_ERROR, (char*)&err, &optlen) < 0 || err != 0) {
        goto error;
    }
    blocking(connfd);
    return connfd;
error:
    closesocket(connfd);
    return socket_errno() > 0 ? -socket_errno() : -1;
}

int Socketpair(int family, int type, int protocol, int sv[2]) {
#ifdef OS_UNIX
    if (family == AF_UNIX) {
        return socketpair(family, type, protocol, sv);
    }
#endif
    if (family != AF_INET || type != SOCK_STREAM) {
        return -1;
    }
    int listenfd, connfd, acceptfd;
    listenfd = connfd = acceptfd = INVALID_SOCKET;
    struct sockaddr_in localaddr;
    socklen_t addrlen = sizeof(localaddr);
    memset(&localaddr, 0, addrlen);
    localaddr.sin_family = AF_INET;
    localaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    localaddr.sin_port = 0;
    // listener
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        goto error;
    }
    if (bind(listenfd, (struct sockaddr*)&localaddr, addrlen) < 0) {
        perror("bind");
        goto error;
    }
    if (listen(listenfd, 1) < 0) {
        perror("listen");
        goto error;
    }
    if (getsockname(listenfd, (struct sockaddr*)&localaddr, &addrlen) < 0) {
        perror("getsockname");
        goto error;
    }
    // connector
    connfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connfd < 0) {
        perror("socket");
        goto error;
    }
    if (connect(connfd, (struct sockaddr*)&localaddr, addrlen) < 0) {
        perror("connect");
        goto error;
    }
    // acceptor
    acceptfd = accept(listenfd, (struct sockaddr*)&localaddr, &addrlen);
    if (acceptfd < 0) {
        perror("accept");
        goto error;
    }

    closesocket(listenfd);
    sv[0] = connfd;
    sv[1] = acceptfd;
    return 0;
error:
    if (listenfd != INVALID_SOCKET) {
        closesocket(listenfd);
    }
    if (connfd != INVALID_SOCKET) {
        closesocket(connfd);
    }
    if (acceptfd != INVALID_SOCKET) {
        closesocket(acceptfd);
    }
    return -1;
}
