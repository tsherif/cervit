#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <stdlib.h>
#include <signal.h>

#define HEADER_BUFFER_SIZE 2048
#define HTTP_OK_HEADER "HTTP/1.1 200 OK\r\n"
#define HTTP_CONTENT_TYPE_KEY "Content-Type: "
#define HTTP_NEWLINE "\r\n"

// TODO(Tarek): Dynamic req/resp buffers
// TODO(Tarek): Threads
// TODO(Tarek): Check for leaving root dir.
// TODO(Tarek): index.html default

int sock;

typedef struct {
    char method[16];
    char url[2048];
} Request;

int skipSpace(char* in, int index) {
    while (1) {
        char c = in[index];

        if (c != ' ' && c != '\t') {
            break;
        }

        ++index;
    }

    return index;
}

char *contentTypeHeader(char* filename) {
    size_t len = strlen(filename);
    char* ext = filename + len - 1;
    
    while (ext != filename && *ext != '.') {
        --ext;
    }

    if (ext == filename) {
        return HTTP_CONTENT_TYPE_KEY "application/octet-stream" HTTP_NEWLINE;
    }

    if (strcmp(ext, ".html") == 0) {
        return HTTP_CONTENT_TYPE_KEY "text/html" HTTP_NEWLINE;
    }

    if (strcmp(ext, ".js") == 0) {
        return HTTP_CONTENT_TYPE_KEY "application/javascript" HTTP_NEWLINE;
    }

    if (strcmp(ext, ".css") == 0) {
        return HTTP_CONTENT_TYPE_KEY "text/css" HTTP_NEWLINE;
    }

    if (strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".jpg") == 0) {
        return HTTP_CONTENT_TYPE_KEY "image/jpeg" HTTP_NEWLINE;
    }

    if (strcmp(ext, ".png") == 0) {
        return HTTP_CONTENT_TYPE_KEY "image/png" HTTP_NEWLINE;
    }

    if (strcmp(ext, ".gif") == 0) {
        return HTTP_CONTENT_TYPE_KEY "image/gif" HTTP_NEWLINE;
    }

    return HTTP_CONTENT_TYPE_KEY "application/octet-stream" HTTP_NEWLINE;
}

void parseRequest(char *requestString, Request* req) {
    int index = skipSpace(requestString, 0);

    char c = requestString[index];
    int i = 0;
    while (c != ' ' && c != '\t') {
        req->method[i++] = c;

        c = requestString[++index];
    }
    req->method[i] = '\0';

    index = skipSpace(requestString, index);

    c = requestString[index];
    i = 1;
    req->url[0] = '.';
    while (c != ' ' && c != '\t') {
        req->url[i++] = c;

        c = requestString[++index];
    }
    req->url[i] = '\0';
}

size_t appendString(char* out, size_t index, char* in) {
    while (*in) {
        out[index++] = *(in++);
    }

    return index;
}

void onClose(void) {
    close(sock);
}

void onInt(int sig) {
    exit(0);
}

int main(int argc, int** argv) {

    atexit(onClose);
    signal(SIGINT, onInt);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Can't create socket");
        return 1;
    }

    struct sockaddr_in addr;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(5000);

    if (bind(sock, (struct sockaddr*) &addr, sizeof(addr))) {
        perror("Can't bind");
        return 1;
    }

    listen(sock, 4);

    char requestBuffer[HEADER_BUFFER_SIZE];

    char* notFound = "HTTP/1.1 404 NOT FOUND\r\n\r\n"
        "<html><body>\n"
        "<h1>File not found!</h1>\n"
        "</body></html>\n";

    char headerBuffer[HEADER_BUFFER_SIZE];
    size_t currentResponseBufferSize = 2 * HEADER_BUFFER_SIZE;
    char* responseBuffer = malloc(currentResponseBufferSize);
    struct stat fileInfo;
    Request req;
    int returnVal = 0;

    while(1) {
        printf("Waiting for connection.\n");
        int connection = accept(sock, 0, 0);

        if (connection == -1) {
            perror("Connection failed");
            continue;
        }

        printf("Got connection.\n");
        int received = recv(connection, requestBuffer, 2047, 0);
        if (received == -1) {
            perror("Failed to receive data.");
            close(connection);
            continue;
        }
        
        requestBuffer[received] = '\0';
        printf("Received request: %s\n", requestBuffer);
        parseRequest(requestBuffer, &req);
        int fd = open(req.url, O_RDONLY);

        char* mimeType = contentTypeHeader(req.url);
        printf("%s\n", mimeType);

        if (fd == -1) {
            write(connection, notFound, strlen(notFound));  
            close(connection);
            continue; 
        }

        returnVal = fstat(fd, &fileInfo);

        if (returnVal == -1) {
            write(connection, notFound, strlen(notFound));  
            close(connection);
            close(fd);
            continue;
        }

        if (fileInfo.st_size + HEADER_BUFFER_SIZE > currentResponseBufferSize) {
            while (fileInfo.st_size + HEADER_BUFFER_SIZE > currentResponseBufferSize) {
                currentResponseBufferSize <<= 1;
            }

            responseBuffer = realloc(responseBuffer, currentResponseBufferSize);
        }

        int index = appendString(responseBuffer, 0, HTTP_OK_HEADER);
        index = appendString(responseBuffer, index, contentTypeHeader(req.url));
        index = appendString(responseBuffer, index, HTTP_NEWLINE);

        returnVal = read(fd, responseBuffer + index, fileInfo.st_size);

        if (returnVal == -1) {
            write(connection, notFound, strlen(notFound));  
            close(connection);
            close(fd);
            continue;
        }

        write(connection, responseBuffer, index + fileInfo.st_size);  
        // char* mimeType = contentTypeHeader(req.url);
        // printf("%s\n", mimeType);

        // int sent = sendfile(connection, fd, NULL, fileInfo.st_size);
        responseBuffer[index + fileInfo.st_size] = '\0';
        printf("Sent response: %s\n", responseBuffer);
        close(connection);
    }

    return 0;
}
