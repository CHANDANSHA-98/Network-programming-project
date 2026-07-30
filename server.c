/*
    server.c - MiniHTTP Server v3 (Windows / Winsock)

    Static file serving (www/) + dynamic routes:
        GET  /time                          -> JSON current server time
        GET  /status                        -> JSON basic server stats (public)
        GET  /calculate?a=5&b=3&op=add       -> JSON calculation result
        GET  /admin                          -> protected dashboard page (Basic Auth)
        GET  /admin/api/status                -> protected JSON: full server/perf details
        GET  /admin/api/visitors              -> protected JSON: recent access log entries
        GET  /admin/api/guestbook             -> protected JSON: guestbook entries
        GET  /guestbook/list                  -> public JSON: guestbook entries (for the page)
        POST /guestbook                       -> saves form submissions

    Also new in v3:
        - Real status codes recorded in access.log (v2 always logged 200, even on errors)
        - Live active-connection / peak-connection tracking
        - Process memory usage (psapi)
        - Maintenance-needed heuristic (error rate, memory, load, uptime)
        - JSON string escaping (v2's JSON endpoints could be broken by quotes in input)
        - /admin.html can no longer be fetched directly as a static file (must go through
          the authenticated /admin route)

    Compile:
        gcc server.c -o server.exe -lws2_32 -lpsapi

    Run:
        .\server.exe
*/

#define _WIN32_WINNT 0x0600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")

#define PORT 8080
#define BUFFER_SIZE 16384
#define WWW_ROOT "www"
#define ADMIN_USER "admin"
#define ADMIN_PASS "1234"

#define MAX_LOG_LINES 50
#define LOG_LINE_MAXLEN 300
#define GUESTBOOK_MAXLEN 700

/* Maintenance thresholds - tweak to taste */
#define MAINT_ERROR_RATE_PCT   20.0
#define MAINT_MEMORY_MB        150.0
#define MAINT_ACTIVE_CONN      50
#define MAINT_UPTIME_SECONDS   (7 * 24 * 3600) /* 1 week */

/* ---------------- Global shared state (protected by critical sections) ---------------- */
CRITICAL_SECTION log_cs;
CRITICAL_SECTION stats_cs;
CRITICAL_SECTION session_cs;
FILE *log_file = NULL;
long total_requests = 0;
long total_errors = 0;          /* status >= 400 */
long active_connections = 0;    /* currently open sockets */
long peak_connections = 0;      /* highest active_connections ever seen */
time_t server_start_time;

/* Single active admin session token. Empty string = nobody logged in.
   Simple by design (one admin, one session) - fine for a class project;
   logging in again simply replaces the token, logging out clears it. */
char admin_session_token[65] = "";

/* ==================================================================================
   UTILITY FUNCTIONS
   ================================================================================== */

const char* get_content_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0)  return "text/css";
    if (strcmp(ext, ".js") == 0)   return "application/javascript";
    if (strcmp(ext, ".txt") == 0)  return "text/plain";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0)  return "image/png";
    if (strcmp(ext, ".svg") == 0)  return "image/svg+xml";
    return "application/octet-stream";
}

/* Case-insensitive string compare (returns 1 if equal, 0 otherwise). Written manually
   since it's called on small header values only. */
int strcasecmp_ci(const char *a, const char *b) {
    if (a[0] == '\0') return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Escape a string for safe embedding inside a JSON string value */
void json_escape(const char *in, char *out, int out_size) {
    int i = 0;
    for (const unsigned char *p = (const unsigned char*)in; *p && i < out_size - 2; p++) {
        if (*p == '"' || *p == '\\') {
            if (i >= out_size - 3) break;
            out[i++] = '\\';
            out[i++] = (char)*p;
        } else if (*p == '\n') {
            if (i >= out_size - 3) break;
            out[i++] = '\\'; out[i++] = 'n';
        } else if (*p == '\r') {
            /* skip */
        } else if (*p < 0x20) {
            /* skip other control chars */
        } else {
            out[i++] = (char)*p;
        }
    }
    out[i] = '\0';
}

/* Thread-safe request logging to access.log. Records the REAL status code
   returned by the handler (v2 always logged 200 here, which made the access
   log useless for spotting errors). */
void log_request(const char *ip, const char *method, const char *path, int status) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", t);

    EnterCriticalSection(&log_cs);
    if (log_file) {
        fprintf(log_file, "[%s] %s \"%s %s\" %d\n", timestr, ip, method, path, status);
        fflush(log_file);
    }
    printf("[%s] %s %s -> %d\n", timestr, method, path, status);
    LeaveCriticalSection(&log_cs);
}

void increment_requests(int status) {
    EnterCriticalSection(&stats_cs);
    total_requests++;
    if (status >= 400) total_errors++;
    LeaveCriticalSection(&stats_cs);
}

void connection_opened(void) {
    EnterCriticalSection(&stats_cs);
    active_connections++;
    if (active_connections > peak_connections) peak_connections = active_connections;
    LeaveCriticalSection(&stats_cs);
}

void connection_closed(void) {
    EnterCriticalSection(&stats_cs);
    if (active_connections > 0) active_connections--;
    LeaveCriticalSection(&stats_cs);
}

/* Format a duration in seconds as "Xd Xh Xm Xs" - used for the uptime / "going time" display */
void format_duration(long seconds, char *out, int out_size) {
    long days = seconds / 86400;
    long hours = (seconds % 86400) / 3600;
    long mins = (seconds % 3600) / 60;
    long secs = seconds % 60;
    if (days > 0)
        snprintf(out, out_size, "%ldd %ldh %ldm %lds", days, hours, mins, secs);
    else if (hours > 0)
        snprintf(out, out_size, "%ldh %ldm %lds", hours, mins, secs);
    else if (mins > 0)
        snprintf(out, out_size, "%ldm %lds", mins, secs);
    else
        snprintf(out, out_size, "%lds", secs);
}

/* Current process working-set memory usage, in megabytes */
double get_memory_mb(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    }
    return 0.0;
}

/* Decode application/x-www-form-urlencoded text: '+' -> space, %XX -> byte */
void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && (a = src[1]) && (b = src[2]) &&
                   isxdigit((unsigned char)a) && isxdigit((unsigned char)b)) {
            char hex[3] = { a, b, 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Minimal Base64 decoder, used only for Basic Auth header (small inputs) */
int base64_decode(const char *in, char *out) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int val = 0, bits = -8, out_len = 0;
    for (const char *p = in; *p && *p != '='; p++) {
        const char *pos = strchr(table, *p);
        if (!pos) continue;
        val = (val << 6) + (int)(pos - table);
        bits += 6;
        if (bits >= 0) {
            out[out_len++] = (char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    out[out_len] = '\0';
    return out_len;
}

/* Send a full HTTP response given a body buffer already prepared */
void send_response(SOCKET sock, int code, const char *status_text,
                    const char *content_type, const char *body, int body_len,
                    int keep_alive, const char *extra_headers) {
    char header[1024];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "%s"
        "\r\n",
        code, status_text, content_type, body_len,
        keep_alive ? "keep-alive" : "close",
        extra_headers ? extra_headers : "");

    send(sock, header, (int)strlen(header), 0);
    if (body_len > 0) send(sock, body, body_len, 0);
}

/* Returns the status code, so callers can log it accurately */
int send_error(SOCKET sock, int code, const char *text, int keep_alive) {
    char body[256];
    snprintf(body, sizeof(body), "<html><body><h1>%d %s</h1></body></html>", code, text);
    send_response(sock, code, text, "text/html", body, (int)strlen(body), keep_alive, NULL);
    return code;
}

/* Serves the styled www/404.html page (falls back to the plain error box if
   that file is somehow missing too) - status code is still a real 404. */
int send_404(SOCKET sock, int keep_alive) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/404.html", WWW_ROOT);
    FILE *file = fopen(filepath, "rb");
    if (!file) return send_error(sock, 404, "Not Found", keep_alive);

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = (char*)malloc(size);
    fread(data, 1, size, file);
    fclose(file);

    send_response(sock, 404, "Not Found", "text/html", data, (int)size, keep_alive, NULL);
    free(data);
    return 404;
}

/* Find a header's integer value in the raw request text */
int get_header_int(const char *request, const char *name, int default_val) {
    const char *p = strstr(request, name);
    if (!p) return default_val;
    p += strlen(name);
    while (*p == ':' || *p == ' ') p++;
    return atoi(p);
}

void get_header_str(const char *request, const char *name, char *out, int out_size) {
    const char *p = strstr(request, name);
    out[0] = '\0';
    if (!p) return;
    p += strlen(name);
    while (*p == ':' || *p == ' ') p++;
    int i = 0;
    while (*p && *p != '\r' && *p != '\n' && i < out_size - 1) out[i++] = *p++;
    out[i] = '\0';
}

/* Read the last `max_lines` lines of a text file into a caller-provided array
   of fixed-width strings (a small ring buffer so memory use stays bounded
   even if the log file grows huge). Returns the number of lines found. */
int read_last_lines(const char *filename, char lines[][LOG_LINE_MAXLEN], int max_lines) {
    FILE *f = fopen(filename, "r");
    if (!f) return 0;

    int count = 0;
    int next = 0; /* next slot to overwrite, ring-buffer style */
    char buf[LOG_LINE_MAXLEN];

    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        if (len == 0) continue;
        strncpy(lines[next], buf, LOG_LINE_MAXLEN - 1);
        lines[next][LOG_LINE_MAXLEN - 1] = '\0';
        next = (next + 1) % max_lines;
        if (count < max_lines) count++;
    }
    fclose(f);

    /* If we wrapped around, rotate so the array reads oldest -> newest */
    if (count == max_lines && next != 0) {
        char tmp[MAX_LOG_LINES][LOG_LINE_MAXLEN];
        for (int i = 0; i < count; i++) {
            strcpy(tmp[i], lines[(next + i) % max_lines]);
        }
        for (int i = 0; i < count; i++) strcpy(lines[i], tmp[i]);
    }
    return count;
}

/* Pull a single cookie's value out of the raw "Cookie:" header, e.g.
   extract_cookie_value(request, "session", out, sizeof(out)) */
void extract_cookie_value(const char *request, const char *name, char *out, int out_size) {
    out[0] = '\0';
    char cookie_header[512];
    get_header_str(request, "Cookie", cookie_header, sizeof(cookie_header));
    if (cookie_header[0] == '\0') return;

    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", name);
    const char *p = strstr(cookie_header, needle);
    if (!p) return;
    p += strlen(needle);

    int i = 0;
    while (*p && *p != ';' && *p != '\r' && *p != '\n' && i < out_size - 1) out[i++] = *p++;
    out[i] = '\0';
}

/* Generate a random 64-char hex session token. Call srand() once in main() first. */
void generate_session_token(char *out, int out_size) {
    static const char hex[] = "0123456789abcdef";
    int i = 0;
    for (; i < out_size - 1; i++) out[i] = hex[rand() % 16];
    out[i] = '\0';
}

/* Session-cookie based auth check, shared by every /admin* route. Replaces
   the old HTTP Basic Auth popup with a real login page + logout. */
int check_session_auth(const char *request) {
    if (admin_session_token[0] == '\0') return 0; /* nobody logged in */
    char cookie_token[65];
    extract_cookie_value(request, "session", cookie_token, sizeof(cookie_token));
    if (cookie_token[0] == '\0') return 0;

    int ok;
    EnterCriticalSection(&session_cs);
    ok = (strcmp(cookie_token, admin_session_token) == 0);
    LeaveCriticalSection(&session_cs);
    return ok;
}

/* HTML admin pages redirect to the login page when not authenticated */
int redirect_to_login(SOCKET sock, int keep_alive) {
    const char *body = "<html><body>Redirecting to login…</body></html>";
    send_response(sock, 302, "Found", "text/html", body, (int)strlen(body),
                  keep_alive, "Location: /admin/login\r\n");
    return 302;
}

/* JSON admin APIs return 401 JSON instead of redirecting, so the dashboard's
   fetch() calls can detect it and redirect via JavaScript */
int send_unauthorized_json(SOCKET sock, int keep_alive) {
    const char *body = "{\"error\": \"unauthorized\"}";
    send_response(sock, 401, "Unauthorized", "application/json", body, (int)strlen(body), keep_alive, NULL);
    return 401;
}

/* ==================================================================================
   ROUTE HANDLERS  (each returns the HTTP status code it sent, for logging)
   ================================================================================== */

int handle_time(SOCKET sock, int keep_alive) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", t);

    char body[128];
    snprintf(body, sizeof(body), "{\"server_time\": \"%s\"}", timestr);
    send_response(sock, 200, "OK", "application/json", body, (int)strlen(body), keep_alive, NULL);
    return 200;
}

int handle_status(SOCKET sock, int keep_alive) {
    long uptime = (long)difftime(time(NULL), server_start_time);
    long req_count, err_count;
    EnterCriticalSection(&stats_cs);
    req_count = total_requests;
    err_count = total_errors;
    LeaveCriticalSection(&stats_cs);

    char body[256];
    snprintf(body, sizeof(body),
        "{\"uptime_seconds\": %ld, \"total_requests\": %ld, \"total_errors\": %ld}",
        uptime, req_count, err_count);
    send_response(sock, 200, "OK", "application/json", body, (int)strlen(body), keep_alive, NULL);
    return 200;
}

/* GET /calculate?a=5&b=3&op=add  -> real server-side calculation */
int handle_calculate(SOCKET sock, const char *query, int keep_alive) {
    double a = 0, b = 0;
    char op[16] = "add";

    const char *p;
    if ((p = strstr(query, "a="))) a = atof(p + 2);
    if ((p = strstr(query, "b="))) b = atof(p + 2);
    if ((p = strstr(query, "op="))) {
        sscanf(p + 3, "%15[^&]", op);
    }

    double result = 0;
    int valid = 1;
    if (strcmp(op, "add") == 0) result = a + b;
    else if (strcmp(op, "sub") == 0) result = a - b;
    else if (strcmp(op, "mul") == 0) result = a * b;
    else if (strcmp(op, "div") == 0) {
        if (b == 0) valid = 0;
        else result = a / b;
    } else valid = 0;

    char body[256];
    if (!valid) {
        snprintf(body, sizeof(body), "{\"error\": \"invalid operation or division by zero\"}");
        send_response(sock, 400, "Bad Request", "application/json", body, (int)strlen(body), keep_alive, NULL);
        return 400;
    }

    snprintf(body, sizeof(body),
        "{\"a\": %.2f, \"b\": %.2f, \"op\": \"%s\", \"result\": %.4f}", a, b, op, result);
    send_response(sock, 200, "OK", "application/json", body, (int)strlen(body), keep_alive, NULL);
    return 200;
}

/* GET /admin/login -> serves the public login page */
int handle_admin_login_page(SOCKET sock, int keep_alive) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/login.html", WWW_ROOT);
    FILE *file = fopen(filepath, "rb");
    if (!file) return send_error(sock, 404, "Not Found", keep_alive);

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = (char*)malloc(size);
    fread(data, 1, size, file);
    fclose(file);

    send_response(sock, 200, "OK", "text/html", data, (int)size, keep_alive, NULL);
    free(data);
    return 200;
}

/* POST /admin/login -> checks username+password, issues a session cookie */
int handle_admin_login_post(SOCKET sock, const char *body, int keep_alive) {
    char user[64] = "", pass[64] = "";
    char decoded_user[64] = "", decoded_pass[64] = "";

    const char *p;
    if ((p = strstr(body, "username="))) sscanf(p + 9, "%63[^&]", user);
    if ((p = strstr(body, "password="))) sscanf(p + 9, "%63[^&]", pass);
    url_decode(decoded_user, user);
    url_decode(decoded_pass, pass);

    if (strcmp(decoded_user, ADMIN_USER) == 0 && strcmp(decoded_pass, ADMIN_PASS) == 0) {
        char token[65];
        generate_session_token(token, sizeof(token));

        EnterCriticalSection(&session_cs);
        strcpy(admin_session_token, token);
        LeaveCriticalSection(&session_cs);

        char headers[160];
        snprintf(headers, sizeof(headers),
            "Location: /admin\r\nSet-Cookie: session=%s; Path=/; HttpOnly\r\n", token);
        const char *ok_body = "<html><body>Logged in, redirecting…</body></html>";
        send_response(sock, 302, "Found", "text/html", ok_body, (int)strlen(ok_body), keep_alive, headers);
        return 302;
    }

    const char *fail_body = "<html><body>Redirecting…</body></html>";
    send_response(sock, 302, "Found", "text/html", fail_body, (int)strlen(fail_body),
                  keep_alive, "Location: /admin/login?error=1\r\n");
    return 302;
}

/* GET /admin/logout -> clears the session and sends the user back to the login page */
int handle_admin_logout(SOCKET sock, int keep_alive) {
    EnterCriticalSection(&session_cs);
    admin_session_token[0] = '\0';
    LeaveCriticalSection(&session_cs);

    const char *body = "<html><body>Logged out, redirecting…</body></html>";
    send_response(sock, 302, "Found", "text/html", body, (int)strlen(body), keep_alive,
                  "Location: /admin/login\r\nSet-Cookie: session=deleted; Path=/; Max-Age=0\r\n");
    return 302;
}

/* GET /admin -> serves the protected dashboard page (www/admin.html) after checking
   the session cookie. admin.html itself calls the JSON endpoints below for live data. */
int handle_admin(SOCKET sock, const char *request, int keep_alive) {
    if (!check_session_auth(request)) return redirect_to_login(sock, keep_alive);

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/admin.html", WWW_ROOT);
    FILE *file = fopen(filepath, "rb");
    if (!file) return send_error(sock, 404, "Not Found", keep_alive);

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = (char*)malloc(size);
    fread(data, 1, size, file);
    fclose(file);

    send_response(sock, 200, "OK", "text/html", data, (int)size, keep_alive, NULL);
    free(data);
    return 200;
}

/* GET /admin/api/status -> full server + performance detail, incl. maintenance verdict */
int handle_admin_api_status(SOCKET sock, const char *request, int keep_alive) {
    if (!check_session_auth(request)) return send_unauthorized_json(sock, keep_alive);

    long uptime = (long)difftime(time(NULL), server_start_time);
    long req_count, err_count, active, peak;
    EnterCriticalSection(&stats_cs);
    req_count = total_requests;
    err_count = total_errors;
    active = active_connections;
    peak = peak_connections;
    LeaveCriticalSection(&stats_cs);

    double error_rate = (req_count > 0) ? (100.0 * (double)err_count / (double)req_count) : 0.0;
    double mem_mb = get_memory_mb();
    double req_per_min = (uptime > 0) ? (60.0 * (double)req_count / (double)uptime) : 0.0;

    char uptime_str[64];
    format_duration(uptime, uptime_str, sizeof(uptime_str));

    time_t now = time(NULL);
    char now_str[64], start_str[64];
    strftime(now_str, sizeof(now_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    strftime(start_str, sizeof(start_str), "%Y-%m-%d %H:%M:%S", localtime(&server_start_time));

    /* Maintenance heuristic: flag if any threshold is breached, and say why */
    char reasons[512] = "";
    int need_maintenance = 0;
    if (error_rate > MAINT_ERROR_RATE_PCT) {
        strcat(reasons, "High error rate; ");
        need_maintenance = 1;
    }
    if (mem_mb > MAINT_MEMORY_MB) {
        strcat(reasons, "High memory usage; ");
        need_maintenance = 1;
    }
    if (active > MAINT_ACTIVE_CONN) {
        strcat(reasons, "High concurrent connection load; ");
        need_maintenance = 1;
    }
    if (uptime > MAINT_UPTIME_SECONDS) {
        strcat(reasons, "No restart in over a week; ");
        need_maintenance = 1;
    }
    if (!need_maintenance) strcpy(reasons, "All systems normal");

    char reasons_esc[600];
    json_escape(reasons, reasons_esc, sizeof(reasons_esc));

    char body[1400];
    snprintf(body, sizeof(body),
        "{"
        "\"server_time\": \"%s\","
        "\"start_time\": \"%s\","
        "\"uptime_seconds\": %ld,"
        "\"uptime_formatted\": \"%s\","
        "\"total_requests\": %ld,"
        "\"total_errors\": %ld,"
        "\"error_rate_percent\": %.2f,"
        "\"requests_per_minute\": %.2f,"
        "\"active_connections\": %ld,"
        "\"peak_connections\": %ld,"
        "\"memory_mb\": %.2f,"
        "\"maintenance_needed\": %s,"
        "\"maintenance_reason\": \"%s\","
        "\"port\": %d"
        "}",
        now_str, start_str, uptime, uptime_str, req_count, err_count,
        error_rate, req_per_min, active, peak, mem_mb,
        need_maintenance ? "true" : "false", reasons_esc, PORT);

    send_response(sock, 200, "OK", "application/json", body, (int)strlen(body), keep_alive, NULL);
    return 200;
}

/* GET /admin/api/visitors -> recent access.log entries as a JSON array.
   Parses lines shaped like: [2026-07-30 10:00:00] 127.0.0.1 "GET /path" 200 */
int handle_admin_api_visitors(SOCKET sock, const char *request, int keep_alive) {
    if (!check_session_auth(request)) return send_unauthorized_json(sock, keep_alive);

    static char lines[MAX_LOG_LINES][LOG_LINE_MAXLEN];
    int count = read_last_lines("access.log", lines, MAX_LOG_LINES);

    char *body = (char*)malloc(MAX_LOG_LINES * (LOG_LINE_MAXLEN + 64) + 32);
    int pos = 0;
    pos += sprintf(body + pos, "[");

    for (int i = count - 1; i >= 0; i--) { /* newest first */
        char timestr[32] = "", ip[64] = "", method[16] = "", path[200] = "";
        int status = 0;
        if (sscanf(lines[i], "[%31[^]]] %63s \"%15s %199[^\"]\" %d",
                   timestr, ip, method, path, &status) == 5) {
            char path_esc[220];
            json_escape(path, path_esc, sizeof(path_esc));
            pos += snprintf(body + pos, LOG_LINE_MAXLEN + 64,
                "%s{\"time\":\"%s\",\"ip\":\"%s\",\"method\":\"%s\",\"path\":\"%s\",\"status\":%d}",
                (pos > 1 ? "," : ""), timestr, ip, method, path_esc, status);
        }
    }
    pos += sprintf(body + pos, "]");

    send_response(sock, 200, "OK", "application/json", body, pos, keep_alive, NULL);
    free(body);
    return 200;
}

/* Shared implementation for reading guestbook.txt into a JSON array.
   Lines are shaped like: Name: Message */
void build_guestbook_json(char *body, int body_size) {
    static char lines[MAX_LOG_LINES][LOG_LINE_MAXLEN];
    int count = read_last_lines("guestbook.txt", lines, MAX_LOG_LINES);

    int pos = 0;
    pos += snprintf(body + pos, body_size - pos, "[");
    for (int i = count - 1; i >= 0 && pos < body_size - 100; i--) { /* newest first */
        char *sep = strstr(lines[i], ": ");
        char name[128] = "Anonymous", message[GUESTBOOK_MAXLEN] = "";
        if (sep) {
            int name_len = (int)(sep - lines[i]);
            if (name_len > (int)sizeof(name) - 1) name_len = sizeof(name) - 1;
            strncpy(name, lines[i], name_len);
            name[name_len] = '\0';
            strncpy(message, sep + 2, sizeof(message) - 1);
        } else {
            strncpy(message, lines[i], sizeof(message) - 1);
        }
        char name_esc[150], msg_esc[GUESTBOOK_MAXLEN + 40];
        json_escape(name, name_esc, sizeof(name_esc));
        json_escape(message, msg_esc, sizeof(msg_esc));
        pos += snprintf(body + pos, body_size - pos,
            "%s{\"name\":\"%s\",\"message\":\"%s\"}",
            (pos > 1 ? "," : ""), name_esc, msg_esc);
    }
    pos += snprintf(body + pos, body_size - pos, "]");
}

int handle_admin_api_guestbook(SOCKET sock, const char *request, int keep_alive) {
    if (!check_session_auth(request)) return send_unauthorized_json(sock, keep_alive);
    char body[16384];
    build_guestbook_json(body, sizeof(body));
    send_response(sock, 200, "OK", "application/json", body, (int)strlen(body), keep_alive, NULL);
    return 200;
}

/* GET /guestbook/list -> public JSON of guestbook entries, so guestbook.html can show them */
int handle_guestbook_list(SOCKET sock, int keep_alive) {
    char body[16384];
    build_guestbook_json(body, sizeof(body));
    send_response(sock, 200, "OK", "application/json", body, (int)strlen(body), keep_alive, NULL);
    return 200;
}

/* POST /guestbook  -> saves name+message submitted from a form */
int handle_guestbook(SOCKET sock, const char *body, int keep_alive) {
    char name[128] = "", message[512] = "";
    char decoded_name[128] = "", decoded_message[512] = "";

    const char *p;
    if ((p = strstr(body, "name="))) sscanf(p + 5, "%127[^&]", name);
    if ((p = strstr(body, "message="))) sscanf(p + 8, "%511[^&]", message);

    url_decode(decoded_name, name);
    url_decode(decoded_message, message);

    if (decoded_name[0] == '\0') strcpy(decoded_name, "Anonymous");

    EnterCriticalSection(&log_cs);
    FILE *gb = fopen("guestbook.txt", "a");
    if (gb) {
        fprintf(gb, "%s: %s\n", decoded_name, decoded_message);
        fclose(gb);
    }
    LeaveCriticalSection(&log_cs);

    char name_esc[300];
    json_escape(decoded_name, name_esc, sizeof(name_esc));

    char response_body[1024];
    snprintf(response_body, sizeof(response_body),
        "<html><body><h1>Thanks, %s!</h1><p>Your message was saved.</p>"
        "<p><a href=\"/guestbook.html\">Back to guestbook</a></p></body></html>",
        decoded_name);
    send_response(sock, 200, "OK", "text/html", response_body, (int)strlen(response_body), keep_alive, NULL);
    return 200;
}

/* Serve a static file from www/ */
int handle_static(SOCKET sock, const char *path, int keep_alive) {
    /* admin.html must only ever be reached through the authenticated /admin route */
    if (strcmp(path, "/admin.html") == 0) {
        return send_error(sock, 403, "Forbidden", keep_alive);
    }

    char filepath[1100];
    if (strcmp(path, "/") == 0)
        snprintf(filepath, sizeof(filepath), "%s/index.html", WWW_ROOT);
    else
        snprintf(filepath, sizeof(filepath), "%s%s", WWW_ROOT, path);

    if (strstr(filepath, "..") != NULL) {
        return send_error(sock, 400, "Bad Request", keep_alive);
    }

    FILE *file = fopen(filepath, "rb");
    if (!file) {
        return send_404(sock, keep_alive);
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = (char*)malloc(size);
    fread(data, 1, size, file);
    fclose(file);

    send_response(sock, 200, "OK", get_content_type(filepath), data, (int)size, keep_alive, NULL);
    free(data);
    return 200;
}

/* ==================================================================================
   CONNECTION HANDLING (runs per-thread, loops for Keep-Alive)
   ================================================================================== */

DWORD WINAPI handle_client(LPVOID arg) {
    SOCKET sock = *(SOCKET*)arg;
    free(arg);
    connection_opened();

    char client_ip[64] = "unknown";
    struct sockaddr_in peer;
    int peer_len = sizeof(peer);
    if (getpeername(sock, (struct sockaddr*)&peer, &peer_len) == 0) {
        strcpy(client_ip, inet_ntoa(peer.sin_addr));
    }

    char buffer[BUFFER_SIZE];

    while (1) {
        int received = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (received <= 0) break; /* client closed connection */
        buffer[received] = '\0';

        char method[16], full_path[1024], version[16];
        if (sscanf(buffer, "%15s %1023s %15s", method, full_path, version) != 3) {
            send_error(sock, 400, "Bad Request", 0);
            break;
        }

        /* Decide keep-alive: HTTP/1.1 defaults to keep-alive unless client says "close" */
        char conn_header[32];
        get_header_str(buffer, "Connection", conn_header, sizeof(conn_header));
        int keep_alive = (strcmp(version, "HTTP/1.1") == 0);
        if (strcasecmp_ci(conn_header, "close")) keep_alive = 0;
        if (strcasecmp_ci(conn_header, "keep-alive")) keep_alive = 1;

        /* Split path and query string on '?' */
        char path[512], query[512] = "";
        char *qmark = strchr(full_path, '?');
        if (qmark) {
            int path_len = (int)(qmark - full_path);
            if (path_len >= (int)sizeof(path)) path_len = sizeof(path) - 1;
            strncpy(path, full_path, path_len);
            path[path_len] = '\0';
            strncpy(query, qmark + 1, sizeof(query) - 1);
        } else {
            strncpy(path, full_path, sizeof(path) - 1);
            path[sizeof(path)-1] = '\0';
        }

        int status = 200;

        if (strcmp(method, "GET") == 0) {
            if (strcmp(path, "/time") == 0) status = handle_time(sock, keep_alive);
            else if (strcmp(path, "/status") == 0) status = handle_status(sock, keep_alive);
            else if (strcmp(path, "/calculate") == 0) status = handle_calculate(sock, query, keep_alive);
            else if (strcmp(path, "/admin") == 0) status = handle_admin(sock, buffer, keep_alive);
            else if (strcmp(path, "/admin/login") == 0) status = handle_admin_login_page(sock, keep_alive);
            else if (strcmp(path, "/admin/logout") == 0) status = handle_admin_logout(sock, keep_alive);
            else if (strcmp(path, "/admin/api/status") == 0) status = handle_admin_api_status(sock, buffer, keep_alive);
            else if (strcmp(path, "/admin/api/visitors") == 0) status = handle_admin_api_visitors(sock, buffer, keep_alive);
            else if (strcmp(path, "/admin/api/guestbook") == 0) status = handle_admin_api_guestbook(sock, buffer, keep_alive);
            else if (strcmp(path, "/guestbook/list") == 0) status = handle_guestbook_list(sock, keep_alive);
            else status = handle_static(sock, path, keep_alive);
        }
        else if (strcmp(method, "POST") == 0) {
            /* Read the body: may need more recv() calls if Content-Length exceeds what we already got */
            int content_length = get_header_int(buffer, "Content-Length", 0);
            char *header_end = strstr(buffer, "\r\n\r\n");
            int already_have = 0;
            char *body_ptr = NULL;

            if (header_end) {
                body_ptr = header_end + 4;
                already_have = received - (int)(body_ptr - buffer);
            }

            char *post_body = (char*)malloc(content_length + 1);
            if (already_have > 0) memcpy(post_body, body_ptr, already_have);
            int total_body = already_have;
            while (total_body < content_length) {
                int more = recv(sock, post_body + total_body, content_length - total_body, 0);
                if (more <= 0) break;
                total_body += more;
            }
            post_body[total_body] = '\0';

            if (strcmp(path, "/guestbook") == 0) status = handle_guestbook(sock, post_body, keep_alive);
            else if (strcmp(path, "/admin/login") == 0) status = handle_admin_login_post(sock, post_body, keep_alive);
            else status = send_error(sock, 404, "Not Found", keep_alive);

            free(post_body);
        }
        else {
            status = send_error(sock, 400, "Only GET and POST are supported", keep_alive);
        }

        increment_requests(status);
        /* The dashboard polls /admin/api/* every few seconds in the background -
           logging those would flood the visitor list with the admin's own
           screen refreshing, not real traffic. Still counted in total_requests. */
        if (strncmp(path, "/admin/api/", 11) != 0) {
            log_request(client_ip, method, full_path, status);
        }

        if (!keep_alive) break;
    }

    connection_closed();
    closesocket(sock);
    return 0;
}

/* ==================================================================================
   MAIN
   ================================================================================== */

int main() {
    WSADATA wsa;
    SOCKET server_socket;
    struct sockaddr_in server_addr, client_addr;
    int client_addr_len = sizeof(client_addr);

    InitializeCriticalSection(&log_cs);
    InitializeCriticalSection(&stats_cs);
    InitializeCriticalSection(&session_cs);
    log_file = fopen("access.log", "a");
    server_start_time = time(NULL);
    srand((unsigned int)time(NULL));

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("Bind failed: %d\n", WSAGetLastError());
        return 1;
    }
    if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
        printf("Listen failed: %d\n", WSAGetLastError());
        return 1;
    }

    printf("========================================\n");
    printf(" MiniHTTP Server v3 running\n");
    printf(" http://localhost:%d/\n", PORT);
    printf(" Admin dashboard -> http://localhost:%d/admin\n", PORT);
    printf(" Admin login -> user: %s  pass: %s\n", ADMIN_USER, ADMIN_PASS);
    printf("========================================\n\n");

    while (1) {
        SOCKET client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket == INVALID_SOCKET) continue;

        SOCKET *sock_ptr = (SOCKET*)malloc(sizeof(SOCKET));
        *sock_ptr = client_socket;
        HANDLE thread = CreateThread(NULL, 0, handle_client, sock_ptr, 0, NULL);
        if (thread) CloseHandle(thread);
    }

    fclose(log_file);
    closesocket(server_socket);
    WSACleanup();
    return 0;
}