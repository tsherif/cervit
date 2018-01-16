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
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

// Forward declare so I don't have to include stdlib.h, string.h
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void *memcpy(void * restrict dest, const void * restrict src, size_t n);
int atexit(void (*func)(void));
void exit(int status);

#define HTTP_OK_HEADER "HTTP/1.1 200 OK\r\n"
#define HTTP_CONTENT_TYPE_KEY "Content-Type: "
#define HTTP_NEWLINE "\r\n"

#define NOT_FOUND "HTTP/1.1 404 NOT FOUND\r\n\r\n<html><body>\n<h1>File not found!</h1>\n</body></html>\n"

#define REQUEST_CHUNK_SIZE 32768

// TODO(Tarek): Directory page
// TODO(Tarek): Check for leaving root dir

typedef struct {
    char* data;
    size_t length;
    size_t size;
} Buffer;

typedef struct {
    Buffer method;
    Buffer url;
} Request;

// The listening socket
int sock;

// Unshared thread variables
long numThreads;
pthread_t *threads;
int *threadIds;
int *connections;
Request *requests;
Buffer *requestBuffers;
Buffer *responseBuffers;

// Shared thread variables
int currentConnection;
pthread_mutex_t currentConnectionLock;
pthread_cond_t currentConnectionWritten;
pthread_cond_t currentConnectionRead;
char currentConnectionWriteDone;
char currentConnectionReadDone;

char* string_skipSpace(char* string) {
    while (1) {
        char c = *string;

        if (c != ' ' && c != '\t') {
            break;
        }

        if (c == '\0') {
            break;
        }

        ++string;
    }

    return string;
}

size_t string_length(const char* string, char* terminators, size_t count) {
    size_t length = 0;
    while (1) {
        char c = string[length];
        for (size_t i = 0; i < count; ++i) {
            if (c == terminators[i]){
                goto done;
            }
        }
        ++length;
    }

    done:
    return length;
}

char string_parseURIHexCode(const char* string) {
    char result = 0;
    char multiplier = 16;
    for (size_t i = 0; i < 2; ++i) {
        char c = string[i];

        if (c >= 'A' && c <= 'F') {
            result += multiplier * (10 + c - 'A');
        } else if (c >= 'a' && c <= 'f') {
            result += multiplier * (10 + c - 'a');
        } else if (c >= '0' && c <= '9') {
            result += multiplier * (c - '0');
        } else {
            return '\0';
        }

        multiplier >>= 4;
    }

    return result;
}

unsigned short string_toUshort(const char* string) {
    ssize_t i = string_length(string, "\0", 1) - 1;
    int multiplier = 1;
    unsigned short result = 0;
    while (i >= 0) {
        char c = string[i];
        if (c < '0' || c > '9') {
            return 0;
        }
        result += (string[i] - '0') * multiplier;
        multiplier *= 10;
        --i;
    }

    return result;
}

char array_equals(char* array1, size_t length1, char* array2, size_t length2) {
    if (length1 != length2) {
        return 0;
    }

    for (size_t i = 0; i < length1; ++i) {
        if (array1[i] != array2[i]) {
            return 0;
        }
    }

    return 1;
}

ssize_t array_find(char* array1, size_t length1, char* array2, size_t length2) {
    if (length1 < length2) {
        return -1;
    }

    size_t length = length1 - length2 + 1;
    for (size_t i = 0; i < length; ++i) {
        if (array_equals(array1 + i, length2, array2, length2)) {
            return i;
        }
    }

    return -1;
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

int buffer_openFile(Buffer* buffer, int flags) {
    // If buffer isn't currently null-terminated, add null
    // in first unused byte for the read.
    if (buffer->data[buffer->length - 1] != '\0') {
        buffer_checkAllocation(buffer, buffer->length + 1);
        buffer->data[buffer->length] = '\0';
    }

    return open(buffer->data, flags);
}

int buffer_statFile(Buffer* buffer, struct stat *fileInfo) {
    // If buffer isn't currently null-terminated, add null
    // in first unused byte for the stat.
    if (buffer->data[buffer->length - 1] != '\0') {
        buffer_checkAllocation(buffer, buffer->length + 1);
        buffer->data[buffer->length] = '\0';
    }

    return stat(buffer->data, fileInfo);
}

ssize_t buffer_appendFromFile(Buffer* buffer, int fd, size_t length) {
    buffer_checkAllocation(buffer, buffer->length + length);
    ssize_t numRead = read(fd, buffer->data + buffer->length, length);
    if (numRead > 0) {
        buffer->length += numRead;
    }
    return numRead;
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

    if (array_equals(filename->data + offset, length, ".html", 5)) {
        return HTTP_CONTENT_TYPE_KEY "text/html" HTTP_NEWLINE;
    }

    if (array_equals(filename->data + offset, length, ".js", 3)) {
        return HTTP_CONTENT_TYPE_KEY "application/javascript" HTTP_NEWLINE;
    }

    if (array_equals(filename->data + offset, length, ".css", 4)) {
        return HTTP_CONTENT_TYPE_KEY "text/css" HTTP_NEWLINE;
    }

    if (array_equals(filename->data + offset, length, ".jpeg", 5) || array_equals(filename->data + offset, length, ".jpg", 4)) {
        return HTTP_CONTENT_TYPE_KEY "image/jpeg" HTTP_NEWLINE;
    }

    if (array_equals(filename->data + offset, length, ".png", 4)) {
        return HTTP_CONTENT_TYPE_KEY "image/png" HTTP_NEWLINE;
    }

    if (array_equals(filename->data + offset, length, ".gif", 4)) {
        return HTTP_CONTENT_TYPE_KEY "image/gif" HTTP_NEWLINE;
    }

    return HTTP_CONTENT_TYPE_KEY "application/octet-stream" HTTP_NEWLINE;
}

// Currently just gets method and URL
void parseRequest(char *requestString, Request* req) {
    req->method.length = 0;
    req->url.length = 0;

    // Get method
    requestString = string_skipSpace(requestString);
    size_t length = string_length(requestString, " \t", 2);
    buffer_appendFromArray(&req->method, requestString, length);
    requestString += length;

    // Get URL
    buffer_appendFromArray(&req->url, ".", 1);

    requestString = string_skipSpace(requestString);
    length = string_length(requestString, "%?# \t", 4);
    buffer_appendFromArray(&req->url, requestString, length);
    requestString += length;
    
    while (*requestString == '%') {
        char c = string_parseURIHexCode(requestString + 1);
        buffer_appendFromArray(&req->url, &c, 1);
        requestString += 3;

        length = string_length(requestString, "%?# \t", 4);
        buffer_appendFromArray(&req->url, requestString, length);
        requestString += length;
    }
}

// Thread main function
void *handleRequest(void* args) {
    int id = *((int *) args);

    struct stat fileInfo;
    int returnVal = 0;
    char requestChunk[REQUEST_CHUNK_SIZE];
    int received = 0;

    while(1) {
        requestBuffers[id].length = 0;
        responseBuffers[id].length = 0;

        // Communication with main thread.
        pthread_mutex_lock(&currentConnectionLock);
        while(!currentConnectionWriteDone) {
            pthread_cond_wait(&currentConnectionWritten, &currentConnectionLock);
        }
        currentConnectionWriteDone = 0;
        connections[id] = currentConnection;
        currentConnectionReadDone = 1;
        pthread_mutex_unlock(&currentConnectionLock);
        pthread_cond_signal(&currentConnectionRead);

        while(1) {
            received = recv(connections[id], requestChunk, REQUEST_CHUNK_SIZE, 0);

            if (received < 1) {
                break;
            }

            // In case it's split between chunks.
            int index = responseBuffers[id].length > 3 ? responseBuffers[id].length - 3 : 0;
            buffer_appendFromArray(&requestBuffers[id], requestChunk, received);
            if (array_find(requestBuffers[id].data + index, requestBuffers[id].length - index, "\r\n\r\n", 4) != -1) {
                break;
            }
        }

        if (received == -1) {
            perror("Failed to receive data");
            close(connections[id]);
            continue;
        }
        
        parseRequest(requestBuffers[id].data, &requests[id]);

        printf("URL %.*s handled by thread %d\n", (int) requests[id].url.length, requests[id].url.data, id);

        returnVal = buffer_statFile(&requests[id].url, &fileInfo);

        if (returnVal == -1) {
            perror("Failed to stat url");
            write(connections[id], NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
            close(connections[id]);
            continue;
        }

        // URL points to directory. Try to send index.html
        if ((fileInfo.st_mode & S_IFMT) == S_IFDIR) {
            if (requests[id].url.data[requests[id].url.length - 1] == '/') {
                buffer_appendFromString(&requests[id].url, "index.html");
            } else {
                buffer_appendFromString(&requests[id].url, "/index.html");
            }
        }

        int fd = buffer_openFile(&requests[id].url, O_RDONLY);

        if (fd == -1) {
            perror("Failed to open file");
            write(connections[id], NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
            close(connections[id]);
            continue; 
        }

        returnVal = fstat(fd, &fileInfo);

        if (returnVal == -1) {
            perror("Failed to stat file");
            write(connections[id], NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
            close(connections[id]);
            close(fd);
            continue;
        }

        buffer_appendFromArray(&responseBuffers[id], HTTP_OK_HEADER, string_length(HTTP_OK_HEADER, "\0", 1));
        buffer_appendFromString(&responseBuffers[id], contentTypeHeader(&requests[id].url));
        buffer_appendFromArray(&responseBuffers[id], HTTP_NEWLINE, string_length(HTTP_NEWLINE, "\0", 4));

        buffer_checkAllocation(&responseBuffers[id], responseBuffers[id].length + fileInfo.st_size);
        returnVal = buffer_appendFromFile(&responseBuffers[id], fd, fileInfo.st_size);

        if (returnVal == -1) {
            perror("Failed to read file");
            write(connections[id], NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
            close(connections[id]);
            close(fd);
            continue;
        }

        write(connections[id], responseBuffers[id].data, responseBuffers[id].length);

        close(connections[id]);
        close(fd);
    }

    
}

void onClose(void) {
    for (int i = 0; i < numThreads; ++i) {
        buffer_delete(&requestBuffers[i]);
        buffer_delete(&responseBuffers[i]);
        buffer_delete(&requests[i].method);
        buffer_delete(&requests[i].url);
        close(connections[i]);
    }
    close(sock);
    free(threads);
    free(threadIds);
    free(connections);
    free(requests);
    free(requestBuffers);
    free(responseBuffers);
}

void onSignal(int sig) {
    exit(0);
}

int main(int argc, char** argv) {

    unsigned short port = 5000;

    numThreads = sysconf(_SC_NPROCESSORS_CONF);

    if (numThreads < 1) {
        numThreads = 1;
    }

    if (argc > 1) {
        unsigned short argPort = string_toUshort(argv[1]);

        if (argPort > 0) {
            port = argPort;
        }
    }

    printf("Starting cervit on port %d using %ld threads\n", port, numThreads);

    atexit(onClose);
    signal(SIGINT, onSignal);
    signal(SIGQUIT, onSignal);
    signal(SIGABRT, onSignal);
    signal(SIGTSTP, onSignal);
    signal(SIGTERM, onSignal);

    threads = malloc(numThreads * sizeof(pthread_t));
    threadIds = malloc(numThreads * sizeof(int));
    connections = malloc(numThreads * sizeof(int));
    requests = malloc(numThreads * sizeof(Request));
    requestBuffers = malloc(numThreads * sizeof(Buffer));
    responseBuffers = malloc(numThreads * sizeof(Buffer));
    
    for (int i = 0; i < numThreads; ++i) {
        threadIds[i] = i;
        pthread_create(&threads[i], NULL, handleRequest, &threadIds[i]);
        buffer_init(&requests[i].method, 16);
        buffer_init(&requests[i].url, 1024);
        buffer_init(&requestBuffers[i], 2048);
        buffer_init(&responseBuffers[i], 512);
    }

    pthread_mutex_init(&currentConnectionLock, NULL);
    pthread_cond_init(&currentConnectionWritten, NULL);
    pthread_cond_init(&currentConnectionRead, NULL);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("Failed to create socket");
        return 1;
    }

    int sockoptTrue = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &sockoptTrue, sizeof(sockoptTrue));

    struct sockaddr_in addr;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*) &addr, sizeof(addr)) == -1) {
        perror("Failed to bind");
        return 1;
    }

    if (listen(sock, SOMAXCONN) == -1) {
        perror("Failed to listen");
        return 1;
    }

    printf("Socket listening\n");

    // Accepting socket
    int connection;

    while(1) {
        connection = accept(sock, 0, 0);

        if (connection == -1) {
            perror("Connection failed");
            continue;
        }

        // Communication with worker threads.
        // Pass accepted socket to worker.
        pthread_mutex_lock(&currentConnectionLock);
        currentConnection = connection;
        currentConnectionWriteDone = 1;
        pthread_mutex_unlock(&currentConnectionLock);
        pthread_cond_signal(&currentConnectionWritten);

        pthread_mutex_lock(&currentConnectionLock);
        while(!currentConnectionReadDone) {
            pthread_cond_wait(&currentConnectionRead, &currentConnectionLock);
        }
        currentConnectionReadDone = 0;
        pthread_mutex_unlock(&currentConnectionLock);
    }

    return 0;
}
