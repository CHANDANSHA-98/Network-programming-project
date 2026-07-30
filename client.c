/*
    client.c - MiniHTTP Client v2 (Windows / Winsock)

    - Connects to a host:port
    - Sends an HTTP GET request you build interactively
    - Prints response headers, saves body to a file
    - After each response, asks if you want to send ANOTHER request on the
      SAME connection (demonstrates HTTP Keep-Alive actually working)

    Compile:
        gcc client.c -o client.exe -lws2_32

    Run:
        .\client.exe
*/

#define _WIN32_WINNT 0x0600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define BUFFER_SIZE 8192

/* Sends one GET request on an already-open socket and prints/saves the response.
   Returns 1 if the connection is still usable afterwards (keep-alive), 0 if closed. */
int do_request(SOCKET sock, const char *host, const char *path, int request_num) {
    char request[1024];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: keep-alive\r\n"
        "User-Agent: MiniHTTPClient/2.0\r\n"
        "\r\n",
        path, host);

    printf("\n--- Sending request #%d ---\n%s", request_num, request);
    send(sock, request, (int)strlen(request), 0);

    char *response = (char*)malloc(1);
    long total_size = 0;
    char buffer[BUFFER_SIZE];
    int bytes;

    /* Read once, then check Content-Length so we know exactly how much more to read
       (important for keep-alive: we must NOT keep reading past this response,
       otherwise we'd swallow bytes belonging to the next response). */
    bytes = recv(sock, buffer, BUFFER_SIZE, 0);
    if (bytes <= 0) { free(response); return 0; }

    response = (char*)realloc(response, bytes + 1);
    memcpy(response, buffer, bytes);
    total_size = bytes;
    response[total_size] = '\0';

    char *body_start = strstr(response, "\r\n\r\n");
    int header_length = body_start ? (int)(body_start - response) : (int)total_size;

    /* find Content-Length header value */
    int content_length = 0;
    char *cl = strstr(response, "Content-Length:");
    if (cl) content_length = atoi(cl + 16);

    int body_have = body_start ? (int)(total_size - header_length - 4) : 0;
    while (body_start && body_have < content_length) {
        bytes = recv(sock, buffer, BUFFER_SIZE, 0);
        if (bytes <= 0) break;
        response = (char*)realloc(response, total_size + bytes + 1);
        memcpy(response + total_size, buffer, bytes);
        total_size += bytes;
        body_have += bytes;
        response[total_size] = '\0';
    }

    printf("\n--- Response headers ---\n%.*s\n", header_length, response);

    if (body_start) {
        char filename[64];
        snprintf(filename, sizeof(filename), "downloaded_output_%d.html", request_num);
        FILE *out = fopen(filename, "wb");
        if (out) {
            fwrite(body_start + 4, 1, total_size - header_length - 4, out);
            fclose(out);
            printf("--- Body saved to %s (%ld bytes) ---\n", filename, total_size - header_length - 4);
        }
    }

    /* Check if the server said it will close the connection */
    int keep_alive = 1;
    if (strstr(response, "Connection: close")) keep_alive = 0;

    free(response);
    return keep_alive;
}

int main() {
    WSADATA wsa;
    SOCKET sock;
    struct addrinfo hints, *result;
    char host[256], port[16], path[512];

    printf("=== MiniHTTP Client v2 ===\n");
    printf("Host (e.g. localhost or example.com): ");
    scanf("%255s", host);
    printf("Port (e.g. 8080 or 80): ");
    scanf("%15s", port);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(host, port, &hints, &result) != 0) {
        printf("Could not resolve host '%s'\n", host);
        WSACleanup();
        return 1;
    }

    sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        printf("Connection failed: %d\n", WSAGetLastError());
        freeaddrinfo(result);
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    freeaddrinfo(result);
    printf("\nConnected to %s:%s\n", host, port);

    int request_num = 1;
    int connection_alive = 1;

    while (connection_alive) {
        printf("\nPath to request (e.g. / or /time or /status): ");
        scanf("%511s", path);

        connection_alive = do_request(sock, host, path, request_num);
        request_num++;

        if (!connection_alive) {
            printf("\nServer closed the connection.\n");
            break;
        }

        printf("\nConnection is still open (Keep-Alive). Send another request on it? (y/n): ");
        char choice;
        scanf(" %c", &choice);
        if (choice != 'y' && choice != 'Y') break;
    }

    closesocket(sock);
    WSACleanup();
    printf("\nConnection closed. Goodbye.\n");
    return 0;
}