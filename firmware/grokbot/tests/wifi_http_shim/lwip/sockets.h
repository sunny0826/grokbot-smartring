#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
int test_getsockname(int, sockaddr *, socklen_t *);
int test_getpeername(int, sockaddr *, socklen_t *);
#define getsockname test_getsockname
#define getpeername test_getpeername

ssize_t test_recv(int, void *, size_t, int);
int test_setsockopt(int, int, int, const void *, socklen_t);
#define recv test_recv
#define setsockopt test_setsockopt

ssize_t test_send(int, const void *, size_t, int);
#define send test_send
