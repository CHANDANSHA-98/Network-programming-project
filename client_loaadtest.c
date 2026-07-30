/*
    client_loadtest.c - Concurrent Load Tester for MiniHTTP Server

    Spawns multiple threads, each sending several HTTP requests to the server,
    and reports total time, successful requests, and requests/second.
    This proves the server's multi-threading actually handles concurrent load.

    Compile:
        gcc client_loadtest.c -o loadtest.exe -lws2_32

    Run:
        .\loadtest.exe
    (make sure server.exe is already running first)
*/

#define _WIN32_WINNT 0x0600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

#define BUFFER_SIZE 4096

char g_host[256];
char g_port[16];
int g_requests_per_thread;

/* Shared counters, protected with InterlockedIncrement (atomic, no need for a mutex) */
volatile LONG successful_requests = 0;
volatile LONG failed_requests = 0;

DWORD WINAPI worker_thread(LPVOID arg) {
    int thread_id = *(int*)arg;
    free(arg);

    struct addrinfo hints, *result;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    for (int i = 0; i < g_requests_per_thread; i++) {
        if (getaddrinfo(g_host, g_port, &hints, &result) != 0) {
            InterlockedIncrement(&failed_requests);
            continue;
        }

        SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
            InterlockedIncrement(&failed_requests);
            freeaddrinfo(result);
            closesocket(sock);
            continue;
        }
        freeaddrinfo(result);

        char request[512];
        snprintf(request, sizeof(request),
            "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", g_host);
        send(sock, request, (int)strlen(request), 0);

        char buffer[BUFFER_SIZE];
        int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);

        if (bytes > 0) {
            InterlockedIncrement(&successful_requests);
        } else {
            InterlockedIncrement(&failed_requests);
        }

        closesocket(sock);
    }

    printf("Thread %d finished.\n", thread_id);
    return 0;
}

int main() {
    int num_threads, requests_per_thread;

    printf("=== MiniHTTP Load Tester ===\n");
    printf("Server host (e.g. localhost): ");
    scanf("%255s", g_host);
    printf("Server port (e.g. 8080): ");
    scanf("%15s", g_port);
    printf("Number of concurrent threads (e.g. 20): ");
    scanf("%d", &num_threads);
    printf("Requests per thread (e.g. 10): ");
    scanf("%d", &requests_per_thread);

    g_requests_per_thread = requests_per_thread;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    HANDLE *threads = (HANDLE*)malloc(num_threads * sizeof(HANDLE));

    printf("\nLaunching %d threads x %d requests each = %d total requests...\n\n",
           num_threads, requests_per_thread, num_threads * requests_per_thread);

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    for (int i = 0; i < num_threads; i++) {
        int *id = (int*)malloc(sizeof(int));
        *id = i + 1;
        threads[i] = CreateThread(NULL, 0, worker_thread, id, 0, NULL);
    }

    WaitForMultipleObjects(num_threads, threads, TRUE, INFINITE);
    QueryPerformanceCounter(&end);

    double elapsed_seconds = (double)(end.QuadPart - start.QuadPart) / freq.QuadPart;
    int total = successful_requests + failed_requests;

    printf("\n========================================\n");
    printf(" LOAD TEST RESULTS\n");
    printf("========================================\n");
    printf(" Total requests:       %d\n", total);
    printf(" Successful:           %ld\n", successful_requests);
    printf(" Failed:               %ld\n", failed_requests);
    printf(" Total time:           %.3f seconds\n", elapsed_seconds);
    printf(" Requests per second:  %.2f\n", total / elapsed_seconds);
    printf("========================================\n");

    for (int i = 0; i < num_threads; i++) CloseHandle(threads[i]);
    free(threads);

    WSACleanup();
    return 0;
}