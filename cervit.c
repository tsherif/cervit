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

#define REQUEST_CHUNK_SIZE 512


// TODO(Tarek): Request of arbitray size
// TODO(Tarek): Replace string funcs with offset/length versions.
// TODO(Tarek): Check for leaving root dir
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

Request req;
Buffer requestBuffer;
Buffer responseBuffer;

size_t string_skipSpace(char* in, size_t index) {
    while (1) {
        char c = in[index];

        if (c != ' ' && c != '\t') {
            break;
        }

        ++index;
    }

    return index;
}

size_t string_length(const char* string, char* terminators, size_t count) {
    size_t length = 0;
    while (1) {
        char c = string[length];
        for (int i = 0; i < count; ++i) {
            if (c == terminators[i]){
                goto done;
            }
        }
        ++length;
    }

    done:
    return length;
}

void buffer_init(Buffer* buffer, size_t size) {
    buffer->data = malloc(size);
    buffer->length = 0;
    buffer->size = size;
}

void buffer_delete(Buffer* buffer) {
    free(buffer->data);
    buffer->data = 0;
    buffer->length = 0;
    buffer->size = 0;
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

void buffer_appendFromArray(Buffer* buffer, const char* array, size_t length) {
    buffer_checkAllocation(buffer, buffer->length + length);
    memcpy(buffer->data + buffer->length, array, length);
    buffer->length += length;
}

void buffer_appendFromString(Buffer* buffer, const char* string) {
    buffer_appendFromArray(buffer, string, string_length(string, "\0", 1));
}

ssize_t buffer_appendFromFile(Buffer* buffer, int fd, size_t length) {
    buffer_checkAllocation(buffer, buffer->length + length);
    ssize_t numRead = read(fd, responseBuffer.data + responseBuffer.length, length);
    if (numRead >= 0) {
        buffer->length += numRead;
    }
    return numRead;
} 

int buffer_equalsArray(Buffer* buffer, size_t bOffset, size_t bLength, char* string, size_t sOffset, size_t sLength) {
    if (bLength != sLength) {
        return 0;
    }

    char *d = buffer->data + bOffset;
    char *s = string + sOffset;

    for (size_t i = 0; i < bLength; ++i) {
        if (d[i] != s[i]) {
            return 0;
        }
    }

    return 1;
}

char *contentTypeHeader(Buffer* filename) {
    size_t offset = filename->length - 1;
    
    while (offset > 0 && filename->data[offset] != '.') {
        --offset;
    }

    if (offset == 0) {
        return HTTP_CONTENT_TYPE_KEY "application/octet-stream" HTTP_NEWLINE;
    }

    size_t length = filename->length - offset;

    if (buffer_equalsArray(filename, offset, length, ".html", 0, 5)) {
        return HTTP_CONTENT_TYPE_KEY "text/html" HTTP_NEWLINE;
    }

    if (buffer_equalsArray(filename, offset, length, ".js", 0, 3)) {
        return HTTP_CONTENT_TYPE_KEY "application/javascript" HTTP_NEWLINE;
    }

    if (buffer_equalsArray(filename, offset, length, ".css", 0, 4)) {
        return HTTP_CONTENT_TYPE_KEY "text/css" HTTP_NEWLINE;
    }

    if (buffer_equalsArray(filename, offset, length, ".jpeg", 0, 5) || buffer_equalsArray(filename, offset, length, ".jpg", 0, 4)) {
        return HTTP_CONTENT_TYPE_KEY "image/jpeg" HTTP_NEWLINE;
    }

    if (buffer_equalsArray(filename, offset, length, ".png", 0, 4)) {
        return HTTP_CONTENT_TYPE_KEY "image/png" HTTP_NEWLINE;
    }

    if (buffer_equalsArray(filename, offset, length, ".gif", 0, 4)) {
        return HTTP_CONTENT_TYPE_KEY "image/gif" HTTP_NEWLINE;
    }

    return HTTP_CONTENT_TYPE_KEY "application/octet-stream" HTTP_NEWLINE;
}

void parseRequest(char *requestString, Request* req) {
    req->method.length = 0;
    req->url.length = 0;

    // Get method
    int index = string_skipSpace(requestString, 0);
    int length = string_length(requestString + index, " \t", 2);
    buffer_appendFromArray(&req->method, requestString + index, length);
    index += length;

    // Get URL
    index = string_skipSpace(requestString, index);
    length = string_length(requestString + index, " \t", 2);

    if (length > 1) {
        buffer_appendFromArray(&req->url, ".", 1);
        buffer_appendFromArray(&req->url, requestString + index, length);
    } else {
        buffer_appendFromArray(&req->url, "./index.html", string_length("./index.html", "\0", 1));
    }
}

void onClose(void) {
    buffer_delete(&requestBuffer);
    buffer_delete(&responseBuffer);
    buffer_delete(&req.method);
    buffer_delete(&req.url);
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

    buffer_init(&req.method, 16);
    buffer_init(&req.url, 1024);
    buffer_init(&requestBuffer, 2048);
    buffer_init(&responseBuffer, 512);

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
    char requestChunk[REQUEST_CHUNK_SIZE];

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

        if (fd == -1) {
            write(connection, NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
            close(connection);
            continue; 
        }

        returnVal = fstat(fd, &fileInfo);

        if (returnVal == -1) {
            write(connection, NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
            close(connection);
            close(fd);
            continue;
        }

        buffer_appendFromArray(&responseBuffer, HTTP_OK_HEADER, string_length(HTTP_OK_HEADER, "\0", 1));
        buffer_appendFromString(&responseBuffer, contentTypeHeader(&req.url));
        buffer_appendFromArray(&responseBuffer, HTTP_NEWLINE, string_length(HTTP_NEWLINE, "\0", 1));

        buffer_checkAllocation(&responseBuffer, responseBuffer.length + fileInfo.st_size);
        returnVal = buffer_appendFromFile(&responseBuffer, fd, fileInfo.st_size);

        if (returnVal == -1) {
            write(connection, NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
            close(connection);
            close(fd);
            continue;
        }


        write(connection, responseBuffer.data, responseBuffer.length);  
        close(connection);
    }

    return 0;
}
