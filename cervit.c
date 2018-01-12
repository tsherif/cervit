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
// TODO(Tarek): Request of arbitray size
// TODO(Tarek): Threads
// TODO(Tarek): Use Buffer struct for all strings
// TODO(Tarek): Parse URL params/hash
// TODO(Tarek): Parse headers.

int sock;

typedef struct {
    char* data;
    size_t length;
    size_t size;
} Buffer;

typedef struct {
    Buffer method;
    Buffer url;
} Request;

Request req = {
    .method = { .size = 16 },
    .url = { .size = 1024 }
};
Buffer requestBuffer = { .size = 2048 };
Buffer responseBuffer = { .size = 512 };

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

void buffer_checkAllocation(Buffer* buffer, size_t requestedSize) {
    if (requestedSize > buffer->size) {
        while (buffer->size < requestedSize) {
            buffer->size <<= 1;
        }
        buffer->data = realloc(buffer->data, buffer->size);
        printf("----Reallocated buffer to: %ld----\n", buffer->size);
    }
}

void buffer_appendFromArray(Buffer* out, char* in, size_t n) {
    buffer_checkAllocation(out, out->length + n);
    memcpy(out->data + out->length, in, n);
    out->length += n;
}

ssize_t buffer_appendFromFile(Buffer* out, int fd, size_t n) {
    buffer_checkAllocation(out, out->length + n);
    ssize_t numRead = read(fd, responseBuffer.data + responseBuffer.length, n);
    if (numRead >= 0) {
        out->length += numRead;
    }
    return numRead;
} 

void parseRequest(char *requestString, Request* req) {
    req->method.length = 0;
    req->url.length = 0;
    int index = skipSpace(requestString, 0);

    int i = index;
    char c = requestString[i];
    int len = 0;
    while (c != ' ' && c != '\t') {
        ++len;
        c = requestString[++i];
    }
    buffer_appendFromArray(&req->method, requestString + index, len);
    buffer_appendFromArray(&req->method, "", 1);
    index += len;

    index = skipSpace(requestString, index);

    i = index;
    c = requestString[i];
    len = 0;
    while (c != ' ' && c != '\t') {
        ++len;
        c = requestString[++i];
    }

    if (len > 1) {
        buffer_appendFromArray(&req->url, ".", 1);
        buffer_appendFromArray(&req->url, requestString + index, len);
    } else {
        buffer_appendFromArray(&req->url, "./index.html", strlen("./index.html"));
    }
    buffer_appendFromArray(&req->method, "", 1);
}

void onClose(void) {
    free(requestBuffer.data);
    free(responseBuffer.data);
    free(req.method.data);
    free(req.url.data);
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

    req.method.data = malloc(req.method.size);
    req.url.data = malloc(req.url.size);
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
        int fd = open(req.url.data, O_RDONLY);

        char* mimeType = contentTypeHeader(req.url.data);
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

        buffer_appendFromArray(&responseBuffer, HTTP_OK_HEADER, strlen(HTTP_OK_HEADER));
        buffer_appendFromArray(&responseBuffer, contentTypeHeader(req.url.data), strlen(contentTypeHeader(req.url.data)));
        buffer_appendFromArray(&responseBuffer, HTTP_NEWLINE, strlen(HTTP_NEWLINE));

        buffer_checkAllocation(&responseBuffer, responseBuffer.length + fileInfo.st_size);
        returnVal = buffer_appendFromFile(&responseBuffer, fd, fileInfo.st_size);

        if (returnVal == -1) {
            write(connection, NOT_FOUND, strlen(NOT_FOUND));  
            close(connection);
            close(fd);
            continue;
        }


        write(connection, responseBuffer.data, responseBuffer.length);  

        responseBuffer.data[responseBuffer.length++] = '\0';
        printf("Sent response: %s\n", responseBuffer.data);
        close(connection);
    }

    return 0;
}
