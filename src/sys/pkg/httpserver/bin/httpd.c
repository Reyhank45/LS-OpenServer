#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>

#define PORT 80
#define WWW_ROOT "/Server/httpserver/www"

void send_response(int client_fd, const char *status, const char *content_type, const void *body, size_t body_len) {
    dprintf(client_fd, "HTTP/1.0 %s\r\n", status);
    dprintf(client_fd, "Content-Type: %s\r\n", content_type);
    dprintf(client_fd, "Content-Length: %zu\r\n", body_len);
    dprintf(client_fd, "\r\n");
    write(client_fd, body, body_len);
}

const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    return "application/octet-stream";
}

void handle_client(int client_fd) {
    char buffer[1024];
    ssize_t n = read(client_fd, buffer, sizeof(buffer)-1);
    if (n <= 0) { close(client_fd); return; }
    buffer[n] = '\0';
    char method[16], path[256];
    if (sscanf(buffer, "%15s %255s", method, path) != 2) {
        close(client_fd);
        return;
    }
    // Only support GET
    if (strcmp(method, "GET") != 0) {
        const char *msg = "Method Not Allowed";
        send_response(client_fd, "405 Method Not Allowed", "text/plain", msg, strlen(msg));
        close(client_fd);
        return;
    }
    // Prevent directory traversal
    if (strstr(path, "..")) {
        const char *msg = "Forbidden";
        send_response(client_fd, "403 Forbidden", "text/plain", msg, strlen(msg));
        close(client_fd);
        return;
    }
    // Default to index.html
    char file_path[512];
    if (strcmp(path, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "%s/index.html", WWW_ROOT);
    } else {
        // strip leading '/'
        snprintf(file_path, sizeof(file_path), "%s%s", WWW_ROOT, path);
    }
    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        const char *msg = "Not Found";
        send_response(client_fd, "404 Not Found", "text/plain", msg, strlen(msg));
        close(client_fd);
        return;
    }
    struct stat st; fstat(fd, &st);
    void *content = malloc(st.st_size);
    read(fd, content, st.st_size);
    close(fd);
    const char *mime = get_mime_type(file_path);
    send_response(client_fd, "200 OK", mime, content, st.st_size);
    free(content);
    close(client_fd);
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(PORT) };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, 5) < 0) { perror("listen"); return 1; }
    printf("httpd listening on port %d\n", PORT);
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) { perror("accept"); continue; }
        handle_client(client_fd);
    }
    return 0;
}
