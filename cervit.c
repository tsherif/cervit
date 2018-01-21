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

#define HTTP_1_1_VERSION "HTTP/1.1"
#define HTTP_1_0_VERSION "HTTP/1.0"
#define HTTP_OK_HEADER "HTTP/1.1 200 OK\r\n"
#define HTTP_CACHE_HEADERS "Server: cervit/0.1\r\nCache-control: no-cache, no-store, must-revalidate\r\nExpires: 0\r\nPragma: no-cache\r\n"
#define HTTP_CONTENT_TYPE_KEY "Content-Type: "
#define HTTP_CONTENT_LENGTH_KEY "Content-Length: "
#define HTTP_NEWLINE "\r\n"

#define HTTP_METHOD_GET 1
#define HTTP_METHOD_HEAD 2
#define HTTP_METHOD_UNSUPPORTED -1

#define BAD_REQUEST "HTTP/1.1 400 BAD REQUEST\r\nContent-Type: text/html\r\nContent-Length: 59\r\n\r\n<html><body>\n<h1>Invalid HTTP request!</h1>\n</body></html>\n"
#define NOT_FOUND "HTTP/1.1 404 NOT FOUND\r\nContent-Type: text/html\r\nContent-Length: 53\r\n\r\n<html><body>\n<h1>File not found!</h1>\n</body></html>\n"
#define TOO_LARGE "HTTP/1.1 431 REQUEST HEADERS TOO LARGE\r\nContent-Type: text/html\r\nContent-Length: 53\r\n\r\n<html><body>\n<h1>Request too large!</h1>\n</body></html>\n"
#define METHOD_NOT_SUPPORTED "HTTP/1.1 501 NOT IMPLEMENTED\r\nContent-Type: text/html\r\nContent-Length: 55\r\n\r\n<html><body>\n<h1>Method not supported!</h1>\n</body></html>\n"
#define VERSION_NOT_SUPPORTED "HTTP/1.1 505 VERSION NOT SUPPORTED\r\nContent-Type: text/html\r\nContent-Length: 70\r\n\r\n<html><body>\n<h1>HTTP version must be 1.1 or 1.0!</h1>\n</body></html>\n"

#define REQUEST_CHUNK_SIZE 32768
#define REQUEST_MAX_SIZE (REQUEST_CHUNK_SIZE * 4)
#define STATIC_STRING_LENGTH(string) (sizeof(string) - 1)

// TODO(Tarek): Can I write from file directly to socket?
// TODO(Tarek): Normalize URI

typedef struct {
    char* data;
    size_t length;
    size_t size;
} Buffer;

typedef struct {
    Buffer method;
    Buffer url;
    Buffer version;
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

size_t string_length(const char* string) {
    size_t length = 0;
    while (string[length] != '\0') {
        ++length;
    }

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
    ssize_t i = string_length(string) - 1;
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

size_t array_skipSpace(char* array, size_t length) {
    size_t i = 0;
    while (i < length) {
        char c = array[i];

        if (c != ' ' && c != '\t') {
            break;
        }

        ++i;
    }

    return i;
}

size_t array_skipHttpNewlines(char* array, size_t length) {
    size_t i = 0;
    while (i < length - 1) {
        if (array[i] == '\r' && array[i + 1] == '\n') {
            i += 2;
        } else {
            break;
        }
    }

    return i;
}

size_t array_findFromByteSet(const char* array, size_t length, char* byteSet, size_t count) {
    size_t i = 0;
    while (i < length) {
        char c = array[i];
        for (size_t j = 0; j < count; ++j) {
            if (c == byteSet[j]){
                return i;
            }
        }
        ++i;
    }

    return length;
}

void buffer_init(Buffer* buffer, size_t size) {
    buffer->data = malloc(size);
    buffer->length = 0;

    if (!buffer->data) {
        fprintf(stderr, "buffer_init: Out of memory\n");
        exit(1);
    }

    buffer->size = size;
}

void buffer_delete(Buffer* buffer) {
    if (buffer->data == 0) {
        return;
    }
    free(buffer->data);
    buffer->data = 0;
    buffer->length = 0;
    buffer->size = 0;
}

void buffer_checkAllocation(Buffer* buffer, size_t requestedSize) {
    if (requestedSize > buffer->size) {
        size_t newSize = buffer->size;
        char* newData;
        while (newSize < requestedSize) {
            newSize <<= 1;
        }
        newData = realloc(buffer->data, newSize);
        if (!newData) {
            fprintf(stderr, "buffer_checkAllocation: Out of memory\n");
            exit(1);
        }
        buffer->data = newData;
        buffer->size = newSize;
    }
}

void buffer_appendFromArray(Buffer* buffer, const char* array, size_t length) {
    buffer_checkAllocation(buffer, buffer->length + length);
    memcpy(buffer->data + buffer->length, array, length);
    buffer->length += length;
}

void buffer_appendFromString(Buffer* buffer, const char* string) {
    buffer_appendFromArray(buffer, string, string_length(string));
}

void buffer_appendFromSizeT(Buffer* buffer, size_t n) {
    size_t pow = 1;
    size_t length = 1;
    while (pow * 10 < n) {
        pow *= 10;
        ++length;
    }

    char result[length];

    size_t i = 0;
    while (pow > 0) {
        char digit = n / pow;
        result[i] = digit + '0';

        n -= digit * pow;
        pow /= 10;
        ++i;
    }

    buffer_appendFromArray(buffer, result, length);
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

int methodCode(Buffer* buffer) {
    if (array_caseEquals(buffer->data, buffer->length, "GET", 3)) {
        return HTTP_METHOD_GET;
    }

    if (array_caseEquals(buffer->data, buffer->length, "HEAD", 4)) {
        return HTTP_METHOD_HEAD;
    }

    return HTTP_METHOD_UNSUPPORTED;
}

// Currently just gets method and URL
int parseRequest(const Buffer* requestBuffer, Request* request) {
    request->method.length = 0;
    request->url.length = 0;
    request->version.length = 0;

    char* requestString = requestBuffer->data;
    size_t requestStringLength = requestBuffer->length;

    // Skip leading newlines
    size_t index = array_skipHttpNewlines(requestString, requestStringLength);
    if (index == requestStringLength) {
        return -1;
    }
    requestString += index;
    requestStringLength -= index;

    // Get method
    index = array_skipSpace(requestString, requestStringLength);
    if (index == requestStringLength) {
        return -1;
    }
    requestString += index;
    requestStringLength -= index;


    index = array_findFromByteSet(requestString, requestStringLength, " \t", 2);
    
    if (index == requestStringLength) {
        return -1;
    }
    buffer_appendFromArray(&request->method, requestString, index);
    requestString += index;
    requestStringLength -= index;


    // Get URL
    buffer_appendFromArray(&request->url, ".", 1);

    index = array_skipSpace(requestString, requestStringLength);
    if (index == requestStringLength) {
        return -1;
    }
    requestString += index;
    requestStringLength -= index;

    index = array_findFromByteSet(requestString, requestStringLength, "%?# \t", 4);
    if (index == requestStringLength) {
        return -1;
    }
    buffer_appendFromArray(&request->url, requestString, index);
    requestString += index;
    requestStringLength -= index;

    while (*requestString == '%') {
        if (requestStringLength < 3) {
            return -1;
        }

        char c = string_parseURIHexCode(requestString + 1);
        if (c > 0) {
            buffer_appendFromArray(&request->url, &c, 1);
            requestString += 3;
            requestStringLength -= 3;
        } else {
            return -1;
        }

        index = array_findFromByteSet(requestString, requestStringLength, "%?# \t", 4);
        if (index == requestStringLength) {
            return -1;
        }
        buffer_appendFromArray(&request->url, requestString, index);
        requestString += index;
        requestStringLength -= index;
    }

    // HTTP version string
    index = array_skipSpace(requestString, requestStringLength);
    if (index == requestStringLength) {
        return -1;
    }
    requestString += index;
    requestStringLength -= index;

    index = array_findFromByteSet(requestString, requestStringLength, " \t\r\n", 4);
    if (index == requestStringLength) {
        return -1;
    }
    buffer_appendFromArray(&request->version, requestString, index);
    requestString += index;
    requestStringLength -= index;

    index = array_skipSpace(requestString, requestStringLength);
    if (index == requestStringLength) {
        return -1;
    }
    requestString += index;
    requestStringLength -= index;

    if (requestStringLength < STATIC_STRING_LENGTH(HTTP_NEWLINE) || !array_equals(requestString, STATIC_STRING_LENGTH(HTTP_NEWLINE), HTTP_NEWLINE, STATIC_STRING_LENGTH(HTTP_NEWLINE))) {
        return -1;
    }

    return 0;
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
    int method = 0;

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

        char validRequest = 0;
        while(1) {
            int received = recv(thread->connection, requestChunk, REQUEST_CHUNK_SIZE, 0);

            if (received == -1) {
                perror("Failed to receive data");
                break;
            }

            // In case it's split between chunks.
            int index = thread->requestBuffer.length > 3 ? thread->requestBuffer.length - 3 : 0;
            buffer_appendFromArray(&thread->requestBuffer, requestChunk, received);

            if (array_find(thread->requestBuffer.data + index, thread->requestBuffer.length - index, "\r\n\r\n", 4) != -1) {
                validRequest = 1;
                break;
            } else if (received < REQUEST_CHUNK_SIZE) {
                // Request ended without header terminator
                write(thread->connection, BAD_REQUEST, STATIC_STRING_LENGTH(BAD_REQUEST));  
                break;
            } else if (thread->requestBuffer.length > REQUEST_MAX_SIZE) {
                write(thread->connection, TOO_LARGE, STATIC_STRING_LENGTH(TOO_LARGE));  
                break;
            }
        }

        if (!validRequest) {
            close(thread->connection);
            continue;
        }

        if (parseRequest(&thread->requestBuffer, &thread->request) == -1) {
            write(thread->connection, BAD_REQUEST, STATIC_STRING_LENGTH(BAD_REQUEST)); 
            close(thread->connection);
            continue;
        }

        method = methodCode(&thread->request.method);

        if (method == HTTP_METHOD_UNSUPPORTED) {
            write(thread->connection, METHOD_NOT_SUPPORTED, STATIC_STRING_LENGTH(METHOD_NOT_SUPPORTED));  
            close(thread->connection);
            continue;
        }

        if (
            !array_caseEquals(thread->request.version.data, thread->request.version.length, HTTP_1_1_VERSION, STATIC_STRING_LENGTH(HTTP_1_1_VERSION)) &&
            !array_caseEquals(thread->request.version.data, thread->request.version.length, HTTP_1_0_VERSION, STATIC_STRING_LENGTH(HTTP_1_0_VERSION))
        ) {
            write(thread->connection, VERSION_NOT_SUPPORTED, STATIC_STRING_LENGTH(VERSION_NOT_SUPPORTED));  
            close(thread->connection);
            continue;
        }

        printf("%.*s %.*s handled by thread %d\n", (int) thread->request.method.length, thread->request.method.data, (int) thread->request.url.length - 1, thread->request.url.data + 1, thread->id);

        returnVal = buffer_statFile(&thread->request.url, &fileInfo);

        if (returnVal == -1) {
            perror("Failed to stat url");
            write(thread->connection, NOT_FOUND, STATIC_STRING_LENGTH(NOT_FOUND));  
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
                buffer_appendFromString(&thread->dirListingBuffer, "<html><body><h1>Directory listing for: ");
                buffer_appendFromArray(&thread->dirListingBuffer, thread->request.url.data + 1, thread->request.url.length - 1); // Skip '.'
                buffer_appendFromString(&thread->dirListingBuffer, "</h1><ul>\n");

                DIR *dir = buffer_openDir(&thread->request.url);

                if (!dir) {
                    perror("Failed to open directory");
                    write(thread->connection, NOT_FOUND, STATIC_STRING_LENGTH(NOT_FOUND));  
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
                    buffer_appendFromString(&thread->dirListingBuffer, "/</a></li>\n");
                }

                for (size_t i = 0; i < fileCount; ++i) {
                    buffer_appendFromString(&thread->dirListingBuffer, "<li><a href=\"");
                    buffer_appendFromArray(&thread->dirListingBuffer, thread->request.url.data + 1, thread->request.url.length - 1); // Skip '.'
                    buffer_appendFromString(&thread->dirListingBuffer, filenames[i]);
                    buffer_appendFromString(&thread->dirListingBuffer, "\">");
                    buffer_appendFromString(&thread->dirListingBuffer, filenames[i]);
                    buffer_appendFromString(&thread->dirListingBuffer, "</a></li>\n");
                }
                buffer_appendFromString(&thread->dirListingBuffer, "</ul></body></html>\n");
                
                buffer_appendFromString(&thread->responseBuffer, HTTP_OK_HEADER HTTP_CACHE_HEADERS "Content-Type: text/html" HTTP_NEWLINE);
                buffer_appendFromString(&thread->responseBuffer, HTTP_CONTENT_LENGTH_KEY);
                buffer_appendFromSizeT(&thread->responseBuffer, thread->dirListingBuffer.length);
                buffer_appendFromString(&thread->responseBuffer, HTTP_NEWLINE HTTP_NEWLINE);

                if (method == HTTP_METHOD_GET) {
                    buffer_appendFromArray(&thread->responseBuffer, thread->dirListingBuffer.data, thread->dirListingBuffer.length);
                }

                write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);  
                close(thread->connection);
                closedir(dir);
                continue;
            }
            
        }

        int fd = buffer_openFile(&thread->request.url, O_RDONLY);

        if (fd == -1) {
            perror("Failed to open file");
            write(thread->connection, NOT_FOUND, STATIC_STRING_LENGTH(NOT_FOUND));  
            close(thread->connection);
            continue; 
        }

        returnVal = fstat(fd, &fileInfo);

        if (returnVal == -1) {
            perror("Failed to stat file");
            write(thread->connection, NOT_FOUND, STATIC_STRING_LENGTH(NOT_FOUND));  
            close(thread->connection);
            close(fd);
            continue;
        }

        buffer_appendFromArray(&thread->responseBuffer, HTTP_OK_HEADER, STATIC_STRING_LENGTH(HTTP_OK_HEADER));
        buffer_appendFromString(&thread->responseBuffer, HTTP_CACHE_HEADERS);
        buffer_appendFromString(&thread->responseBuffer, HTTP_CONTENT_TYPE_KEY);
        buffer_appendFromString(&thread->responseBuffer, contentTypeHeader(&thread->request.url));
        buffer_appendFromString(&thread->responseBuffer, HTTP_NEWLINE);
        buffer_appendFromString(&thread->responseBuffer, HTTP_CONTENT_LENGTH_KEY);
        buffer_appendFromSizeT(&thread->responseBuffer, fileInfo.st_size);
        buffer_appendFromString(&thread->responseBuffer, HTTP_NEWLINE HTTP_NEWLINE);

        if (method == HTTP_METHOD_GET) {
            returnVal = buffer_appendFromFile(&thread->responseBuffer, fd, fileInfo.st_size);

            if (returnVal == -1) {
                perror("Failed to read file");
                write(thread->connection, NOT_FOUND, STATIC_STRING_LENGTH(NOT_FOUND));  
                close(thread->connection);
                close(fd);
                continue;
            }
        }

        write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);

        close(thread->connection);
        close(fd);
    }
}

void onClose(void) {
    if (!threads) {
        return;
    }

    for (int i = 0; i < numThreads; ++i) {
        pthread_cancel(threads[i].thread);
        buffer_delete(&threads[i].requestBuffer);
        buffer_delete(&threads[i].responseBuffer);
        buffer_delete(&threads[i].request.method);
        buffer_delete(&threads[i].request.url);
        buffer_delete(&threads[i].request.version);
        buffer_delete(&threads[i].dirListingBuffer);
        buffer_delete(&threads[i].dirnameBuffer);
        buffer_delete(&threads[i].filenameBuffer);
        close(threads[i].connection);
    }
    free(threads);

    pthread_mutex_destroy(&currentConnectionLock);
    pthread_cond_destroy(&currentConnectionWritten);
    pthread_cond_destroy(&currentConnectionRead);

    close(sock);
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

    int errorCode = 0;
    threads = malloc(numThreads * sizeof(Thread));

    if (!threads) {
        fprintf(stderr, "Failed to allocate thread array\n");
        return 1;
    }

    for (int i = 0; i < numThreads; ++i) {
        threads[i].id = i;
        errorCode = pthread_create(&threads[i].thread, NULL, handleRequest, &threads[i]);
        if (errorCode) {
            fprintf(stderr, "Failed to create thread. Error code: %d", errorCode);
            return 1;
        }
        buffer_init(&threads[i].request.method, 16);
        buffer_init(&threads[i].request.url, 1024);
        buffer_init(&threads[i].request.version, 16);
        buffer_init(&threads[i].requestBuffer, 2048);
        buffer_init(&threads[i].responseBuffer, 1024);
        buffer_init(&threads[i].dirListingBuffer, 512);
        buffer_init(&threads[i].dirnameBuffer, 512);
        buffer_init(&threads[i].filenameBuffer, 512);
    }

    errorCode = pthread_mutex_init(&currentConnectionLock, NULL);
    if (errorCode) {
        fprintf(stderr, "Failed to create mutex. Error code: %d", errorCode);
        return 1;
    }

    errorCode = pthread_cond_init(&currentConnectionWritten, NULL);
    if (errorCode) {
        fprintf(stderr, "Failed to create write condition. Error code: %d", errorCode);
        return 1;
    }

    errorCode = pthread_cond_init(&currentConnectionRead, NULL);
    if (errorCode) {
        fprintf(stderr, "Failed to create read condition. Error code: %d", errorCode);
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("Failed to create socket");
        return 1;
    }

    int sockoptTrue = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &sockoptTrue, sizeof(sockoptTrue)) == -1) {
        perror("Failed to set socket options");
        return 1;
    }

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
