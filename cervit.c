///////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2018 Tarek Sherif
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
///////////////////////////////////////////////////////////////////////////////////

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

#define NOT_FOUND "HTTP/1.1 404 NOT FOUND\r\n\r\n<html><body>\n<h1>File not found!</h1>\n</body></html>\n"

// TODO(Tarek): Check for leaving root dir
// TODO(Tarek): index.html default
// TODO(Tarek): Threads
// TODO(Tarek): Use Buffer struct for all strings

int sock;

typedef struct {
    char* data;
    size_t length;
    size_t size;
} Buffer;

typedef struct {
    char method[16];
    char url[2048];
} Request;

Buffer requestBuffer = { .size = 2048 };
Buffer responseBuffer = { .size = 64 };

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

void checkBufferAllocation(Buffer* buffer, size_t requestedSize) {
    if (requestedSize > buffer->size) {
        while (buffer->size < requestedSize) {
            buffer->size <<= 1;
        }
        buffer->data = realloc(buffer->data, buffer->size);
        printf("----Reallocated buffer to: %ld----\n", buffer->size);
    }
}

void appendString(Buffer* out, char* in) {
    size_t len = 0;
    
    while (in[len++]);
    checkBufferAllocation(out, out->length + len);

    while (*in) {
        out->data[out->length++] = *(in++);
    }
} 

void onClose(void) {
    free(requestBuffer.data);
    free(responseBuffer.data);
    close(sock);
}

void onSignal(int sig) {
    exit(0);
}

int main(int argc, int** argv) {

    atexit(onClose);
    signal(SIGINT, onSignal);
    signal(SIGQUIT, onSignal);
    signal(SIGABRT, onSignal);
    signal(SIGTSTP, onSignal);
    signal(SIGTERM, onSignal);

    requestBuffer.data = malloc(requestBuffer.size);
    responseBuffer.data = malloc(responseBuffer.size);

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

    struct stat fileInfo;
    Request req;
    int returnVal = 0;

    while(1) {
        requestBuffer.length = 0;
        responseBuffer.length = 0;

        printf("Waiting for connection.\n");
        int connection = accept(sock, 0, 0);

        if (connection == -1) {
            perror("Connection failed");
            continue;
        }

        printf("Got connection.\n");
        int received = recv(connection, requestBuffer.data, requestBuffer.size - 1, 0);
        if (received == -1) {
            perror("Failed to receive data.");
            close(connection);
            continue;
        }
        
        requestBuffer.length = received;
        requestBuffer.data[received] = '\0';
        printf("Received request: %s\n", requestBuffer.data);
        parseRequest(requestBuffer.data, &req);
        int fd = open(req.url, O_RDONLY);

        char* mimeType = contentTypeHeader(req.url);
        printf("%s\n", mimeType);

        if (fd == -1) {
            write(connection, NOT_FOUND, strlen(NOT_FOUND));  
            close(connection);
            continue; 
        }

        returnVal = fstat(fd, &fileInfo);

        if (returnVal == -1) {
            write(connection, NOT_FOUND, strlen(NOT_FOUND));  
            close(connection);
            close(fd);
            continue;
        }

        appendString(&responseBuffer, HTTP_OK_HEADER);
        appendString(&responseBuffer, contentTypeHeader(req.url));
        appendString(&responseBuffer, HTTP_NEWLINE);

        checkBufferAllocation(&responseBuffer, responseBuffer.length + fileInfo.st_size);
        returnVal = read(fd, responseBuffer.data + responseBuffer.length, fileInfo.st_size);

        if (returnVal == -1) {
            write(connection, NOT_FOUND, strlen(NOT_FOUND));  
            close(connection);
            close(fd);
            continue;
        }

        responseBuffer.length += fileInfo.st_size;

        write(connection, responseBuffer.data, responseBuffer.length);  

        responseBuffer.data[responseBuffer.length++] = '\0';
        printf("Sent response: %s\n", responseBuffer.data);
        close(connection);
    }

    return 0;
}
