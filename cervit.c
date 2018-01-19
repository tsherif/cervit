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
#include <dirent.h>

// Forward declare so I don't have to include stdlib.h, string.h
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void *memcpy(void * restrict dest, const void * restrict src, size_t n);
int atexit(void (*func)(void));
void exit(int status);

#define HTTP_OK_HEADER "HTTP/1.1 200 OK\r\n"
#define HTTP_CACHE_HEADERS "Server: cervit/0.1\r\nCache-control: no-cache, no-store, must-revalidate\r\nExpires: 0\r\nPragma: no-cache\r\n"
#define HTTP_CONTENT_TYPE_KEY "Content-Type: "
#define HTTP_NEWLINE "\r\n"

#define NOT_FOUND "HTTP/1.1 404 NOT FOUND\r\n\r\n<html><body>\n<h1>File not found!</h1>\n</body></html>\n"

#define REQUEST_CHUNK_SIZE 32768

// TODO(Tarek): Content-Length response header
// TODO(Tarek): HEAD response
// TODO(Tarek): Not supported response for non-get, non-head requests
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

typedef struct {
    pthread_t thread;
    Request request;
    Buffer requestBuffer;
    Buffer responseBuffer;
    Buffer dirListingBuffer;
    Buffer dirnameBuffer;
    Buffer filenameBuffer;
    int id;
    int connection;
} Thread;

// The listening socket
int sock;

// Unshared thread variables
long numThreads;
Thread* threads;


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

int string_compare(char* string1, char* string2) {
    size_t i = 0;
    while (string1[i] || string2[i]) {
        if (!string1[i]) {
            // Name 1 is shorter
            return -1;
        }

        if (!string2[i]) {
            // Name 2 is shorter
            return 1;
        }

        int cmp = string1[i] - string2[i];

        if (cmp != 0) {
            return cmp;
        }

        ++i;
    }

    return 0;
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

char array_caseEquals(char* array1, size_t length1, char* array2, size_t length2) {
    if (length1 != length2) {
        return 0;
    }

    char toLower = 'a' - 'A';

    for (size_t i = 0; i < length1; ++i) {
        char c1 = array1[i];
        char c2 = array2[i];

        if (c1 >= 'A' && c1 <= 'Z') {
            c1 += toLower;
        }

        if (c2 >= 'A' && c2 <= 'Z') {
            c2 += toLower;
        }

        if (c1 != c2) {
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

// If buffer isn't currently null-terminated, add null
// in first unused byte.
void buffer_externalNull(Buffer* buffer) {
    // If buffer isn't currently null-terminated, add null
    // in first unused byte for the read.
    if (buffer->data[buffer->length - 1] != '\0') {
        buffer_checkAllocation(buffer, buffer->length + 1);
        buffer->data[buffer->length] = '\0';
    }
}

int buffer_openFile(Buffer* buffer, int flags) {
    buffer_externalNull(buffer);

    return open(buffer->data, flags);
}

int buffer_statFile(Buffer* buffer, struct stat *fileInfo) {
    buffer_externalNull(buffer);

    return stat(buffer->data, fileInfo);
}

DIR* buffer_openDir(Buffer* buffer) {
    buffer_externalNull(buffer);

    return opendir(buffer->data);
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
        return "application/octet-stream";
    }

    char* extension = filename->data + offset;
    size_t length = filename->length - offset;

    /////////////
    // Text
    /////////////
    if (array_caseEquals(extension, length, ".html", 5) || array_caseEquals(extension, length, ".htm", 4)) {
        return "text/html";
    }

    if (array_caseEquals(extension, length, ".js", 3)) {
        return "application/javascript";
    }

    if (array_caseEquals(extension, length, ".css", 4)) {
        return "text/css";
    }

    if (array_caseEquals(extension, length, ".xml", 4)) {
        return "text/xml";
    }

    if (array_caseEquals(extension, length, ".json", 5)) {
        return "application/json";
    }

    if (array_caseEquals(extension, length, ".txt", 4)) {
        return "text/plain";
    }

    /////////////
    // Images
    /////////////
    if (array_caseEquals(extension, length, ".jpeg", 5) || array_caseEquals(extension, length, ".jpg", 4)) {
        return "image/jpeg";
    }

    if (array_caseEquals(extension, length, ".png", 4)) {
        return "image/png";
    }

    if (array_caseEquals(extension, length, ".gif", 4)) {
        return "image/gif";
    }

    if (array_caseEquals(extension, length, ".bmp", 4)) {
        return "image/bmp";
    }

    if (array_caseEquals(extension, length, ".svg", 4)) {
        return "image/svg+xml";
    }

    /////////////
    // Video
    /////////////
    if (array_caseEquals(extension, length, ".ogv", 4)) {
        return "video/ogg";
    }

    if (array_caseEquals(extension, length, ".mp4", 4)) {
        return "video/mp4";
    }

    if (array_caseEquals(extension, length, ".mpg", 4) || array_caseEquals(extension, length, ".mpeg", 5)) {
        return "video/mpeg";
    }

    if (array_caseEquals(extension, length, ".mov", 4)) {
        return "video/quicktime";
    }

    /////////////
    // Audio
    /////////////
    if (array_caseEquals(extension, length, ".ogg", 4)) {
        return "application/ogg";
    }

    if (array_caseEquals(extension, length, ".oga", 4)) {
        return "audio/ogg";
    }

    if (array_caseEquals(extension, length, ".mp3", 4)) {
        return "audio/mpeg";
    }

    if (array_caseEquals(extension, length, ".wav", 4)) {
        return "audio/wav";
    }

    return "application/octet-stream";
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
        if (c > 0) {
            buffer_appendFromArray(&req->url, &c, 1);
            requestString += 3;
        } else {
            ++requestString;
        }

        length = string_length(requestString, "%?# \t", 4);
        buffer_appendFromArray(&req->url, requestString, length);
        requestString += length;
    }
}

void sortNameList(char** list, size_t length) {
    char *current;
    for (size_t i = 1; i < length; ++i) {
        current = list[i];
        size_t j = i;
        while (j > 0) {
            if (string_compare(current, list[j - 1]) < 0) {
                list[j] = list[j - 1];
            } else {
                break;
            }
            --j;
        }
        list[j] = current;
    }
}

// Thread main function
void *handleRequest(void* args) {
    Thread* thread = (Thread*) args;

    struct stat fileInfo;
    int returnVal = 0;
    char requestChunk[REQUEST_CHUNK_SIZE];
    int received = 0;

    while(1) {
        thread->requestBuffer.length = 0;
        thread->responseBuffer.length = 0;

        // Communication with main thread.
        pthread_mutex_lock(&currentConnectionLock);
        while(!currentConnectionWriteDone) {
            pthread_cond_wait(&currentConnectionWritten, &currentConnectionLock);
        }
        currentConnectionWriteDone = 0;
        thread->connection = currentConnection;
        currentConnectionReadDone = 1;
        pthread_mutex_unlock(&currentConnectionLock);
        pthread_cond_signal(&currentConnectionRead);

        while(1) {
            received = recv(thread->connection, requestChunk, REQUEST_CHUNK_SIZE, 0);

            if (received < 1) {
                break;
            }

            // In case it's split between chunks.
            int index = thread->responseBuffer.length > 3 ? thread->responseBuffer.length - 3 : 0;
            buffer_appendFromArray(&thread->requestBuffer, requestChunk, received);
            if (array_find(thread->requestBuffer.data + index, thread->requestBuffer.length - index, "\r\n\r\n", 4) != -1) {
                break;
            }
        }

        if (received == -1) {
            perror("Failed to receive data");
            close(thread->connection);
            continue;
        }
        
        parseRequest(thread->requestBuffer.data, &thread->request);

        printf("URL %.*s handled by thread %d\n", (int) thread->request.url.length, thread->request.url.data, thread->id);

        returnVal = buffer_statFile(&thread->request.url, &fileInfo);

        if (returnVal == -1) {
            perror("Failed to stat url");
            write(thread->connection, NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
            close(thread->connection);
            continue;
        }

        // Handle directory
        if ((fileInfo.st_mode & S_IFMT) == S_IFDIR) {
            if (thread->request.url.data[thread->request.url.length - 1] != '/') {
                buffer_appendFromString(&thread->request.url, "/");
            }

            // Try to send index.html
            size_t baseLength = thread->request.url.length;
            buffer_appendFromString(&thread->request.url, "index.html");

            returnVal = buffer_statFile(&thread->request.url, &fileInfo);

            // Otherwise send directory listing.
            if (returnVal == -1) {
                thread->dirListingBuffer.length = 0;
                thread->dirnameBuffer.length = 0;
                thread->filenameBuffer.length = 0;
                thread->request.url.length = baseLength;
                buffer_appendFromString(&thread->dirListingBuffer, HTTP_OK_HEADER HTTP_CACHE_HEADERS "Content-Type: text/html" HTTP_NEWLINE HTTP_NEWLINE);
                buffer_appendFromString(&thread->dirListingBuffer, "<html><body><h1>Directory listing for: ");
                buffer_appendFromArray(&thread->dirListingBuffer, thread->request.url.data + 1, thread->request.url.length - 1); // Skip '.'
                buffer_appendFromString(&thread->dirListingBuffer, "</h1><ul>\n");

                DIR *dir = buffer_openDir(&thread->request.url);

                if (!dir) {
                    perror("Failed to open directory");
                    write(thread->connection, NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
                    close(thread->connection);
                    continue;
                }
                
                struct dirent entry;
                struct dirent* entryp;

                size_t dirCount = 0;
                size_t fileCount = 0;

                readdir_r(dir, &entry, &entryp);
                while (entryp) {
                    if (array_equals(entry.d_name, 2, ".", 2) || array_equals(entry.d_name, 3, "..", 3)) {
                        readdir_r(dir, &entry, &entryp);
                        continue;
                    }

                    // Remove added entry name
                    thread->request.url.length = baseLength;
                    
                    buffer_appendFromString(&thread->request.url, entry.d_name);

                    if (entry.d_type == DT_DIR) {
                        buffer_appendFromString(&thread->dirnameBuffer, entry.d_name); 
                        buffer_appendFromArray(&thread->dirnameBuffer, "", 1); 
                        ++dirCount;
                    } else if (entry.d_type == DT_REG) {
                        buffer_appendFromString(&thread->filenameBuffer, entry.d_name); 
                        buffer_appendFromArray(&thread->filenameBuffer, "", 1); 
                        ++fileCount;
                    }

                    readdir_r(dir, &entry, &entryp);
                }

                char* directoryNames[dirCount];
                char* filenames[fileCount];
                size_t currentFile = 1;
                size_t currentDir = 1;

                directoryNames[0] = thread->dirnameBuffer.data;
                filenames[0] = thread->filenameBuffer.data;

                char* current = thread->dirnameBuffer.data;
                char* end = thread->dirnameBuffer.data + thread->dirnameBuffer.length;
                while (current != end && currentDir < dirCount) {
                    if (*current == '\0') {
                        directoryNames[currentDir] = current + 1;
                        ++currentDir;
                    }
                    ++current;
                }

                current = thread->filenameBuffer.data;
                end = thread->filenameBuffer.data + thread->filenameBuffer.length;
                while (current != end && currentFile < fileCount) {
                    if (*current == '\0') {
                        filenames[currentFile] = current + 1;
                        ++currentFile;
                    }
                    ++current;
                }

                sortNameList(directoryNames, dirCount);
                sortNameList(filenames, fileCount);

                thread->request.url.length = baseLength;
                
                for (size_t i = 0; i < dirCount; ++i) {
                    buffer_appendFromString(&thread->dirListingBuffer, "<li><a href=\"");
                    buffer_appendFromArray(&thread->dirListingBuffer, thread->request.url.data + 1, thread->request.url.length - 1); // Skip '.'
                    buffer_appendFromString(&thread->dirListingBuffer, directoryNames[i]);
                    buffer_appendFromString(&thread->dirListingBuffer, "/\">");
                    buffer_appendFromString(&thread->dirListingBuffer, directoryNames[i]);
                    buffer_appendFromString(&thread->dirListingBuffer, "/</a></li>");
                }

                for (size_t i = 0; i < fileCount; ++i) {
                    buffer_appendFromString(&thread->dirListingBuffer, "<li><a href=\"");
                    buffer_appendFromArray(&thread->dirListingBuffer, thread->request.url.data + 1, thread->request.url.length - 1); // Skip '.'
                    buffer_appendFromString(&thread->dirListingBuffer, filenames[i]);
                    buffer_appendFromString(&thread->dirListingBuffer, "\">");
                    buffer_appendFromString(&thread->dirListingBuffer, filenames[i]);
                    buffer_appendFromString(&thread->dirListingBuffer, "</a></li>");
                }
                buffer_appendFromString(&thread->dirListingBuffer, "</ul></body></html>" HTTP_NEWLINE HTTP_NEWLINE);

                write(thread->connection, thread->dirListingBuffer.data, thread->dirListingBuffer.length);  
                close(thread->connection);
                closedir(dir);
                continue;
            }
            
        }

        int fd = buffer_openFile(&thread->request.url, O_RDONLY);

        if (fd == -1) {
            perror("Failed to open file");
            write(thread->connection, NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
            close(thread->connection);
            continue; 
        }

        returnVal = fstat(fd, &fileInfo);

        if (returnVal == -1) {
            perror("Failed to stat file");
            write(thread->connection, NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
            close(thread->connection);
            close(fd);
            continue;
        }

        buffer_appendFromArray(&thread->responseBuffer, HTTP_OK_HEADER, string_length(HTTP_OK_HEADER, "\0", 1));
        buffer_appendFromString(&thread->responseBuffer, HTTP_CACHE_HEADERS);
        buffer_appendFromString(&thread->responseBuffer, HTTP_CONTENT_TYPE_KEY);
        buffer_appendFromString(&thread->responseBuffer, contentTypeHeader(&thread->request.url));
        buffer_appendFromString(&thread->responseBuffer, HTTP_NEWLINE);
        buffer_appendFromArray(&thread->responseBuffer, HTTP_NEWLINE, string_length(HTTP_NEWLINE, "\0", 4));

        buffer_checkAllocation(&thread->responseBuffer, thread->responseBuffer.length + fileInfo.st_size);
        returnVal = buffer_appendFromFile(&thread->responseBuffer, fd, fileInfo.st_size);

        if (returnVal == -1) {
            perror("Failed to read file");
            write(thread->connection, NOT_FOUND, string_length(NOT_FOUND, "\0", 1));  
            close(thread->connection);
            close(fd);
            continue;
        }

        write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);

        close(thread->connection);
        close(fd);
    }
}

void onClose(void) {
    close(sock);
    pthread_mutex_destroy(&currentConnectionLock);
    pthread_cond_destroy(&currentConnectionWritten);
    pthread_cond_destroy(&currentConnectionRead);

    for (int i = 0; i < numThreads; ++i) {
        pthread_cancel(threads[i].thread);
        buffer_delete(&threads[i].requestBuffer);
        buffer_delete(&threads[i].responseBuffer);
        buffer_delete(&threads[i].request.method);
        buffer_delete(&threads[i].request.url);
        buffer_delete(&threads[i].dirListingBuffer);
        buffer_delete(&threads[i].dirnameBuffer);
        buffer_delete(&threads[i].filenameBuffer);
        close(threads[i].connection);
    }

    free(threads);
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

    int initErrors = 0;
    int errorCode = 0;
    threads = malloc(numThreads * sizeof(Thread));

    for (int i = 0; i < numThreads; ++i) {
        threads[i].id = i;
        errorCode = pthread_create(&threads[i].thread, NULL, handleRequest, &threads[i]);
        if (errorCode) {
            fprintf(stderr, "Failed to create thread. Error code: %d", errorCode);
            ++initErrors;
        }
        buffer_init(&threads[i].request.method, 16);
        buffer_init(&threads[i].request.url, 1024);
        buffer_init(&threads[i].requestBuffer, 2048);
        buffer_init(&threads[i].responseBuffer, 1024);
        buffer_init(&threads[i].dirListingBuffer, 512);
        buffer_init(&threads[i].dirnameBuffer, 512);
        buffer_init(&threads[i].filenameBuffer, 512);
    }

    errorCode = pthread_mutex_init(&currentConnectionLock, NULL);
    if (errorCode) {
        fprintf(stderr, "Failed to create mutex. Error code: %d", errorCode);
        ++initErrors;
    }

    errorCode = pthread_cond_init(&currentConnectionWritten, NULL);
    if (errorCode) {
        fprintf(stderr, "Failed to create write condition. Error code: %d", errorCode);
        ++initErrors;
    }

    errorCode = pthread_cond_init(&currentConnectionRead, NULL);
    if (errorCode) {
        fprintf(stderr, "Failed to create read condition. Error code: %d", errorCode);
        ++initErrors;
    }

    if (initErrors) {
        return 1;
    }

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
