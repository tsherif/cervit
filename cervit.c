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
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <dirent.h>
#include <time.h>
#include <stdint.h>

// Forward declare so I don't have to include stdlib.h, string.h
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void *memcpy(void * restrict dest, const void * restrict src, size_t n);
int atexit(void (*func)(void));
void exit(int status);

#ifndef VERSION
#define VERSION "0.0"
#endif

#define HTTP_1_1_VERSION "HTTP/1.1"
#define HTTP_OK_HEADER "HTTP/1.1 200 OK\r\n"
#define HTTP_CACHE_HEADERS "Server: cervit/" VERSION "\r\nCache-control: no-cache, no-store, must-revalidate\r\nExpires: 0\r\nPragma: no-cache\r\n"
#define HTTP_CONTENT_TYPE_KEY "Content-Type: "
#define HTTP_CONTENT_LENGTH_KEY "Content-Length: "
#define HTTP_DATE_KEY "Date: "
#define HTTP_NEWLINE "\r\n"
#define HTTP_END_HEADER HTTP_NEWLINE HTTP_NEWLINE

#define HTTP_METHOD_GET 1
#define HTTP_METHOD_HEAD 2
#define HTTP_METHOD_UNSUPPORTED -1

#define BAD_REQUEST_HEADERS "HTTP/1.1 400 BAD REQUEST\r\nServer: cervit/" VERSION "\r\nContent-Type: text/html\r\nContent-Length: 59\r\n"
#define BAD_REQUEST_BODY "<html><body>\n<h1>Invalid HTTP request!</h1>\n</body></html>\n"
#define NOT_FOUND_HEADERS "HTTP/1.1 404 NOT FOUND\r\nServer: cervit/" VERSION "\r\nContent-Type: text/html\r\nContent-Length: 53\r\n"
#define NOT_FOUND_BODY "<html><body>\n<h1>File not found!</h1>\n</body></html>\n"
#define METHOD_NOT_SUPPORTED_HEADERS "HTTP/1.1 501 NOT IMPLEMENTED\r\nServer: cervit/" VERSION "\r\nContent-Type: text/html\r\nContent-Length: 55\r\n"
#define METHOD_NOT_SUPPORTED_BODY "<html><body>\n<h1>Method not supported!</h1>\n</body></html>\n"
#define VERSION_NOT_SUPPORTED_HEADERS "HTTP/1.1 505 VERSION NOT SUPPORTED\r\nServer: cervit/" VERSION "\r\nContent-Type: text/html\r\nContent-Length: 63\r\n"
#define VERSION_NOT_SUPPORTED_BODY "<html><body>\n<h1>HTTP version must be 1.1!</h1>\n</body></html>\n"

#define TRANSFER_CHUNK_SIZE 32768
#define REQUEST_MAX_SIZE (TRANSFER_CHUNK_SIZE * 4)
#define STATIC_STRING_LENGTH(string) ((int32_t) sizeof(string) - 1)

#define BYTESET_TOKEN_END " \t\r\n"
#define BYTESET_PATH_END "?#" BYTESET_TOKEN_END
#define BYTESET_HEADER_KEY_END ":" BYTESET_TOKEN_END

const char* DAY_STRINGS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char* MONTH_STRINGS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// Dynamically allocated array.
// .data: stored data
// .length: number of bytes currently stored
// .size: number of bytes allocated to the array
typedef struct {
    char* data;
    int32_t length;
    int32_t size;
} Buffer;

// Information about the HTTP request
typedef struct {
    Buffer method;
    Buffer path;
    Buffer version;
} Request;

// Per-thread variables
// .thread: The pthread object
// .request: Parsed data from the request the thread is handling
// .requestBuffer: Buffer struct to load incoming request stream
// .responseBuffer: Buffer struct to build response headers
// .dirListingBuffer: Buffer struct to build a directory listing response
// .dirnameBuffer: Buffer to hold directory names so they can be sorted
// .filenameBuffer: Buffer to hold filenames so they can be sorted
// .id: Id number of the thread
// .connection: Accepted socket that the thread is handling
typedef struct {
    pthread_t thread;
    Request request;
    Buffer requestBuffer;
    Buffer responseBuffer;
    Buffer dirListingBuffer;
    Buffer dirnameBuffer;
    Buffer filenameBuffer;
    int32_t id;
    int32_t connection;
} Thread;

// The listening socket
int32_t sock;

// Array of thread structs
int32_t numThreads;
Thread* threads;


// Shared thread control objects
int32_t currentConnection;
pthread_mutex_t currentConnectionLock;
pthread_cond_t currentConnectionWritten;
pthread_cond_t currentConnectionRead;
int8_t currentConnectionWriteDone;
int8_t currentConnectionReadDone;

///////////////////////////////////////////////
// STRINGS
// A "string" is a null-terminated sequence
// of bytes.
///////////////////////////////////////////////

// Count the number of characters before the 
// first null
int32_t string_length(const char* string) {
    int32_t length = 0;
    while (string[length] != '\0') {
        ++length;
    }

    return length;
}

// Compare two strings for alphabetical ordering. Result
// < 0 means string1 comes first. Result > 0 means string
// 2 should come first, 0 means they're the same.
int32_t string_compare(char* string1, char* string2) {
    int32_t i = 0;
    while (string1[i] || string2[i]) {
        if (!string1[i]) {
            // Name 1 is shorter
            return -1;
        }

        if (!string2[i]) {
            // Name 2 is shorter
            return 1;
        }

        int32_t cmp = string1[i] - string2[i];

        if (cmp != 0) {
            return cmp;
        }

        ++i;
    }

    return 0;
}

// Parse the character value from a string of 
// two hexadecimal digits
char string_parseURIHexCode(const char* string) {
    char result = 0;
    int32_t multiplier = 16;
    for (int32_t i = 0; i < 2; ++i) {
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

// Convert a string of decimal digits
// to a uint32_t value.
uint32_t string_toUint(const char* string) {
    int32_t i = string_length(string) - 1;
    uint32_t multiplier = 1;
    uint32_t result = 0;
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

/////////////////////////////////////////////
// ARRAYS
// An "array" is a sequence of bytes and a 
// length value indicating the number 
// characters in the sequence.
/////////////////////////////////////////////

// Check if two array are the same sequence of bytes.
int8_t array_equals(char* array1, int32_t length1, char* array2, int32_t length2) {
    if (length1 != length2) {
        return 0;
    }

    for (int32_t i = 0; i < length1; ++i) {
        if (array1[i] != array2[i]) {
            return 0;
        }
    }

    return 1;
}

// Check if two array are the same sequence of bytes, disregarding
// case for alphabetical values [A-Za-z].
int8_t array_caseEquals(char* array1, int32_t length1, char* array2, int32_t length2) {
    if (length1 != length2) {
        return 0;
    }

    char toLower = 'a' - 'A';

    for (int32_t i = 0; i < length1; ++i) {
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

// Find the first byte in an array that isn't a space (' ') or 
// tab('\t'), return the index.
int32_t array_skipSpace(char* array, int32_t length) {
    int32_t i = 0;
    while (i < length) {
        char c = array[i];

        if (c != ' ' && c != '\t') {
            break;
        }

        ++i;
    }

    return i;
}

// Check if the beginning of the array is an acceptable
// HTTP newline, '\r\n' or '\n' (RFC 7230, 3.5). If so,
// return the number of characters that make up the newline,
// else return 0.
int32_t array_isHttpNewline(char* array, int32_t length) {
    if (length < 1) {
        return 0;
    }

    if (*array == '\n') {
        return 1;
    }

    if (length > 1 && array[0] == '\r' && array[1] == '\n') {
        return 2;
    }

    return 0;
}

// Check if the beginning of the array is an acceptable
// pair of HTTP newlines. If so, return the number of 
// characters that make up the newline pair, else return
// 0.
int32_t array_isHttpHeaderEnd(char* array, int32_t length) {
    if (length < 2) {
        return 0;
    }

    int32_t i = array_isHttpNewline(array, length);

    if (i == 0) {
        return 0;
    }

    int32_t j = array_isHttpNewline(array + i, length - i);

    if (j == 0) {
        return 0;
    }

    return i + j;
}

// Skip over any leading HTTP newlines
int32_t array_skipHttpNewlines(char* array, int32_t length) {
    int32_t i = 0;
    int32_t count = array_isHttpNewline(array, length);
    while (count && i < length) {
        i += count;
        count = array_isHttpNewline(array + i, length - i);
    }

    return i;
}

// Find if array2 exists as a subarray of array1. If so, return index,
// else return -1.
int32_t array_find(char* array1, int32_t length1, char* array2, int32_t length2) {
    if (length1 < length2) {
        return -1;
    }

    int32_t length = length1 - length2 + 1;
    for (int32_t i = 0; i < length; ++i) {
        if (array_equals(array1 + i, length2, array2, length2)) {
            return i;
        }
    }

    return -1;
}

// Find first occurance in the array of any of the characters in byteSet. If found,
// return index, else return -1.
int32_t array_findFromByteSet(const char* array, int32_t length, char* byteSet, int32_t count) {
    int32_t i = 0;
    while (i < length) {
        char c = array[i];
        for (int32_t j = 0; j < count; ++j) {
            if (c == byteSet[j]){
                return i;
            }
        }
        ++i;
    }

    return -1;
}

// Increment an array's pointer by increment and adjust length accordingly. If
// succesful return 0, else return -1.
int8_t array_incrementPointer(char** array, int32_t* length, int32_t increment) {
    if (increment >= *length) {
        return -1;
    }

    *array += increment;
    *length -= increment;

    return 0;
}

///////////////////////////////////////////////
// BUFFERS
// Buffers are dynamic arrays that will
// automatically allocate the memory required
// to store data appended to them.
///////////////////////////////////////////////

// Initialize a buffer to the given size.
void buffer_init(Buffer* buffer, int32_t size) {
    buffer->data = malloc(size);
    buffer->length = 0;

    if (!buffer->data) {
        fprintf(stderr, "buffer_init: Out of memory\n");
        exit(1);
    }

    buffer->size = size;
}

// Deallocate memory associated with a buffer.
void buffer_delete(Buffer* buffer) {
    if (buffer->data == 0) {
        return;
    }
    free(buffer->data);
    buffer->data = 0;
    buffer->length = 0;
    buffer->size = 0;
}

// Check if buffer is large enough to hold the requested amount 
// of data. If not, reallocate buffer with enough memory.
void buffer_checkAllocation(Buffer* buffer, int32_t requestedSize) {
    if (requestedSize > buffer->size) {
        int32_t newSize = buffer->size;
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

// Append bytes from array to end of buffer.
void buffer_appendFromArray(Buffer* buffer, const char* array, int32_t length) {
    buffer_checkAllocation(buffer, buffer->length + length);
    memcpy(buffer->data + buffer->length, array, length);
    buffer->length += length;
}

// Append non-null bytes from string to end of buffer.
void buffer_appendFromString(Buffer* buffer, const char* string) {
    buffer_appendFromArray(buffer, string, string_length(string));
}

// Convert unsigned int to array of digit characters
// and append to end of buffer.
void buffer_appendFromUint(Buffer* buffer, uint32_t n) {
    uint32_t pow = 1;
    uint32_t length = 1;
    while (pow * 10 <= n) {
        pow *= 10;
        ++length;
    }

    char result[length];

    uint32_t i = 0;
    while (pow > 0) {
        char digit = n / pow;
        result[i] = digit + '0';

        n -= digit * pow;
        pow /= 10;
        ++i;
    }

    buffer_appendFromArray(buffer, result, length);
}

// Decode any percent-encoded characters
// from the buffer.
int8_t buffer_URIHexDecode(Buffer* buffer) {
    char* path = buffer->data;
    int32_t length = buffer->length;
    
    int32_t readIndex = 0;
    int32_t writeIndex = 0;

    while (readIndex < length) {
        if (path[readIndex] != '%') {
            path[writeIndex] = path[readIndex];
            ++readIndex;
            ++writeIndex;
            continue;
        }

        if (readIndex + 2 >= length) {
            return -1;
        }

        char c = string_parseURIHexCode(path + readIndex + 1);
        if (c > 0) {
            path[writeIndex] = c;
            readIndex += 3;
            ++writeIndex;
        } else {
            return -1;
        }
    }

    buffer->length = writeIndex;

    return 0;
}

// Remove path '.' and '..' from a path.
void buffer_removePathDotSegments(Buffer* buffer) {
    // Skip ./ prefix
    char* path = buffer->data + 2;
    int32_t length = buffer-> length - 2;
    
    int32_t readIndex = 0;
    int32_t writeIndex = 0;
    char c1, c2, c3;

    while (readIndex < length) {
        c1 = path[readIndex];

        // Only interested in segments beginning with '.'
        if (c1 != '.' || (readIndex > 0 && path[readIndex - 1] != '/')) {
            path[writeIndex] = path[readIndex];
            ++readIndex;
            ++writeIndex;
            continue;
        }

        if (readIndex + 1 == length) {
            break;
        }

        c2 = path[readIndex + 1];
        
        if (c2 == '/') {
            readIndex += 2;
        } else if (c2 == '.') { 
            if (readIndex + 2 == length) {
                break;
            }

            c3 = path[readIndex + 2];

            if (c3 == '/') {
                readIndex += 3;

                if (writeIndex > 0) {
                    --writeIndex;
                    while (writeIndex > 0 && path[writeIndex - 1] != '/') {
                        --writeIndex;
                    } 
                }
            } else {
                path[writeIndex] = path[readIndex];
                path[writeIndex + 1] = path[readIndex + 1];
                readIndex += 2;
                writeIndex += 2;
            }

        } else {
            path[writeIndex] = path[readIndex];
            ++readIndex;
            ++writeIndex;
        }

    }

    buffer->length = writeIndex + 2;
}

// Append the current date and time (GMT)
// to the buffer.
void buffer_appendDate(Buffer* buffer) {
    time_t t = time(NULL);
    struct tm date;
    gmtime_r(&t, &date);

    buffer_checkAllocation(buffer, buffer->length + 29);

    buffer_appendFromString(buffer, DAY_STRINGS[date.tm_wday]);
    buffer_appendFromString(buffer, ", ");
    buffer_appendFromUint(buffer, date.tm_mday);
    buffer_appendFromString(buffer, " ");
    buffer_appendFromString(buffer, MONTH_STRINGS[date.tm_mon]);
    buffer_appendFromString(buffer, " ");
    buffer_appendFromUint(buffer, date.tm_year + 1900);
    buffer_appendFromString(buffer, " ");

    if (date.tm_hour < 10) {
        buffer_appendFromString(buffer, "0");
    }
    buffer_appendFromUint(buffer, date.tm_hour);
    buffer_appendFromString(buffer, ":");

    if (date.tm_min < 10) {
        buffer_appendFromString(buffer, "0");
    }
    buffer_appendFromUint(buffer, date.tm_min);
    buffer_appendFromString(buffer, ":"); 

    if (date.tm_sec < 10) {
        buffer_appendFromString(buffer, "0");
    }
    buffer_appendFromUint(buffer, date.tm_sec);

    buffer_appendFromString(buffer, " GMT"); 
}

// Set the buffer's contents to an error response base on the 
// given headers and body.
void buffer_errorResponse(Buffer* buffer, const char* headers, const char* body) {
    buffer->length = 0;

    buffer_appendFromString(buffer, headers);
    buffer_appendFromString(buffer, HTTP_DATE_KEY);
    buffer_appendDate(buffer);
    buffer_appendFromString(buffer, HTTP_END_HEADER);
    buffer_appendFromString(buffer, body);
}

// If buffer isn't currently null-terminated, add null
// in first unused byte. Useful when interacting
// with system calls that expect null-termination.
void buffer_externalNull(Buffer* buffer) {
    // If buffer isn't currently null-terminated, add null
    // in first unused byte for the read.
    if (buffer->data[buffer->length - 1] != '\0') {
        buffer_checkAllocation(buffer, buffer->length + 1);
        buffer->data[buffer->length] = '\0';
    }
}

// Open file whose name is stored in buffer.
int32_t buffer_openFile(Buffer* buffer, int32_t flags) {
    buffer_externalNull(buffer);

    return open(buffer->data, flags);
}

// Stat file whose name is stored in buffer.
int32_t buffer_statFile(Buffer* buffer, struct stat *fileInfo) {
    buffer_externalNull(buffer);

    return stat(buffer->data, fileInfo);
}

// Open directory whose name is stored in buffer.
DIR* buffer_openDir(Buffer* buffer) {
    buffer_externalNull(buffer);

    return opendir(buffer->data);
}

// Guess at content stype based on file extension
// of file name stored in filename.
char *contentTypeHeader(Buffer* filename) {
    int32_t offset = filename->length - 1;
    
    while (offset > 0 && filename->data[offset] != '.') {
        --offset;
    }

    if (offset == 0) {
        return "application/octet-stream";
    }

    char* extension = filename->data + offset;
    int32_t length = filename->length - offset;

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

int32_t methodCode(Buffer* buffer) {
    if (array_caseEquals(buffer->data, buffer->length, "GET", 3)) {
        return HTTP_METHOD_GET;
    }

    if (array_caseEquals(buffer->data, buffer->length, "HEAD", 4)) {
        return HTTP_METHOD_HEAD;
    }

    return HTTP_METHOD_UNSUPPORTED;
}

// Validate and parse the incoming request string.
int8_t parseRequest(const Buffer* requestBuffer, Request* request) {
    request->method.length = 0;
    request->path.length = 0;
    request->version.length = 0;

    char* requestString = requestBuffer->data;
    int32_t requestStringLength = requestBuffer->length;

    // Skip leading newlines
    int32_t index = array_skipHttpNewlines(requestString, requestStringLength);
    if (array_incrementPointer(&requestString, &requestStringLength, index) == -1) {
        return -1;
    }

    // Get method
    index = array_skipSpace(requestString, requestStringLength);
    if (array_incrementPointer(&requestString, &requestStringLength, index) == -1) {
        return -1;
    }

    index = array_findFromByteSet(requestString, requestStringLength, BYTESET_TOKEN_END, STATIC_STRING_LENGTH(BYTESET_TOKEN_END));
    if (index == -1) {
        return -1;
    }
    buffer_appendFromArray(&request->method, requestString, index);
    requestString += index;
    requestStringLength -= index;


    // Get URL
    buffer_appendFromArray(&request->path, ".", 1);

    index = array_skipSpace(requestString, requestStringLength);
    if (array_incrementPointer(&requestString, &requestStringLength, index) == -1) {
        return -1;
    }

    index = array_findFromByteSet(requestString, requestStringLength, BYTESET_PATH_END, STATIC_STRING_LENGTH(BYTESET_PATH_END));
    if (index == -1) {
        return -1;
    }
    buffer_appendFromArray(&request->path, requestString, index);
    requestString += index;
    requestStringLength -= index;

    if (buffer_URIHexDecode(&request->path) == -1) {
        return -1;
    }
    buffer_removePathDotSegments(&request->path);

    index = array_findFromByteSet(requestString, requestStringLength, BYTESET_TOKEN_END, STATIC_STRING_LENGTH(BYTESET_TOKEN_END));
    if (array_incrementPointer(&requestString, &requestStringLength, index) == -1) {
        return -1;
    }

    // HTTP version string
    index = array_skipSpace(requestString, requestStringLength);
    if (array_incrementPointer(&requestString, &requestStringLength, index) == -1) {
        return -1;
    }

    index = array_findFromByteSet(requestString, requestStringLength, BYTESET_TOKEN_END, STATIC_STRING_LENGTH(BYTESET_TOKEN_END));
    if (index == -1) {
        return -1;
    }
    buffer_appendFromArray(&request->version, requestString, index);
    requestString += index;
    requestStringLength -= index;

    index = array_skipSpace(requestString, requestStringLength);
    if (array_incrementPointer(&requestString, &requestStringLength, index) == -1) {
        return -1;
    }

    if (!array_isHttpNewline(requestString, requestStringLength)) {
        return -1;
    }

    // Find "Host" header. Required to respond with 400 if not found (RFC 2616, 14.23)
    int8_t hostFound = 0;
    while (!hostFound && index < requestStringLength) {
        int32_t index = array_skipHttpNewlines(requestString, requestStringLength);
        if (array_incrementPointer(&requestString, &requestStringLength, index) == -1) {
            return -1;
        }

        index = array_skipSpace(requestString, requestStringLength);
        if (array_incrementPointer(&requestString, &requestStringLength, index) == -1) {
            return -1;
        }

        // Start of header key
        index = array_findFromByteSet(requestString, requestStringLength, BYTESET_HEADER_KEY_END, STATIC_STRING_LENGTH(BYTESET_HEADER_KEY_END));
        if (index == -1) {
            return -1;
        }
        if (array_caseEquals(requestString, index, "Host", STATIC_STRING_LENGTH("Host"))) {
            hostFound = 1;
        }
        if (array_incrementPointer(&requestString, &requestStringLength, index) == -1) {
            return -1;
        }

        if (*requestString != ':') {
            return -1;
        }

        index = array_findFromByteSet(requestString, requestStringLength, HTTP_NEWLINE, STATIC_STRING_LENGTH(HTTP_NEWLINE));
        if (index == -1) {
            return -1;
        } 
        if (array_incrementPointer(&requestString, &requestStringLength, index) == -1) {
            return -1;
        }

        if (!array_isHttpNewline(requestString, requestStringLength)) {
            return -1;
        }

        if (hostFound || array_isHttpHeaderEnd(requestString, requestStringLength)) {
            break;
        }
    }

    if (!hostFound) {
        return -1;
    }

    return 0;
}

// Sort a list of strings. Used for directory 
// listing responses.
void sortNameList(char** list, int32_t length) {
    char *current;
    for (int32_t i = 1; i < length; ++i) {
        current = list[i];
        int32_t j = i;
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

// Main thread function to handle incoming
// requests.
void *handleRequest(void* args) {
    Thread* thread = (Thread*) args;

    struct stat fileInfo;
    int32_t returnVal = 0;
    int32_t method = 0;
    char requestChunk[TRANSFER_CHUNK_SIZE];

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
            int32_t received = recv(thread->connection, requestChunk, TRANSFER_CHUNK_SIZE, 0);

            if (received == -1) {
                perror("Failed to receive data");
                break;
            }

            // In case it's split between chunks.
            int32_t index = thread->requestBuffer.length > 3 ? thread->requestBuffer.length - 3 : 0;
            buffer_appendFromArray(&thread->requestBuffer, requestChunk, received);

            for (int32_t i = index; i < thread->requestBuffer.length; ++i) {
                if (array_isHttpHeaderEnd(thread->requestBuffer.data + i, thread->requestBuffer.length - i)) {
                    validRequest = 1;
                    break;
                }
            }

            if (validRequest) {
                break;
            } else if (received < TRANSFER_CHUNK_SIZE) {
                // Request ended without header terminator
                buffer_errorResponse(&thread->responseBuffer, BAD_REQUEST_HEADERS, BAD_REQUEST_BODY);
                write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);
                break;
            } else if (thread->requestBuffer.length > REQUEST_MAX_SIZE) {
                buffer_errorResponse(&thread->responseBuffer, BAD_REQUEST_HEADERS, BAD_REQUEST_BODY);
                write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);
                break;
            }
        }

        if (!validRequest) {
            close(thread->connection);
            continue;
        }

        if (parseRequest(&thread->requestBuffer, &thread->request) == -1) {
            buffer_errorResponse(&thread->responseBuffer, BAD_REQUEST_HEADERS, BAD_REQUEST_BODY);
            write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);
            close(thread->connection);
            continue;
        }

        method = methodCode(&thread->request.method);

        if (method == HTTP_METHOD_UNSUPPORTED) {
            buffer_errorResponse(&thread->responseBuffer, METHOD_NOT_SUPPORTED_HEADERS, METHOD_NOT_SUPPORTED_BODY);
            write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);
            close(thread->connection);
            continue;
        }

        if (!array_caseEquals(thread->request.version.data, thread->request.version.length, HTTP_1_1_VERSION, STATIC_STRING_LENGTH(HTTP_1_1_VERSION))) {
            buffer_errorResponse(&thread->responseBuffer, VERSION_NOT_SUPPORTED_HEADERS, VERSION_NOT_SUPPORTED_BODY);
            write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);
            close(thread->connection);
            continue;
        }

        printf("%.*s %.*s handled by thread %d\n", (int32_t) thread->request.method.length, thread->request.method.data, (int32_t) thread->request.path.length - 1, thread->request.path.data + 1, thread->id);

        returnVal = buffer_statFile(&thread->request.path, &fileInfo);

        if (returnVal == -1) {
            buffer_errorResponse(&thread->responseBuffer, NOT_FOUND_HEADERS, NOT_FOUND_BODY);
            write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);  
            close(thread->connection);
            continue;
        }

        // Handle directory
        if ((fileInfo.st_mode & S_IFMT) == S_IFDIR) {
            if (thread->request.path.data[thread->request.path.length - 1] != '/') {
                buffer_appendFromString(&thread->request.path, "/");
            }

            // Try to send index.html
            int32_t baseLength = thread->request.path.length;
            buffer_appendFromString(&thread->request.path, "index.html");

            returnVal = buffer_statFile(&thread->request.path, &fileInfo);

            // Otherwise send directory listing.
            if (returnVal == -1) {
                thread->dirListingBuffer.length = 0;
                thread->dirnameBuffer.length = 0;
                thread->filenameBuffer.length = 0;
                thread->request.path.length = baseLength;
                buffer_appendFromString(&thread->dirListingBuffer, "<html><body><h1>Directory listing for: ");
                buffer_appendFromArray(&thread->dirListingBuffer, thread->request.path.data + 1, thread->request.path.length - 1); // Skip '.'
                buffer_appendFromString(&thread->dirListingBuffer, "</h1><ul>\n");

                DIR *dir = buffer_openDir(&thread->request.path);

                if (!dir) {
                    perror("Failed to open directory");
                    buffer_errorResponse(&thread->responseBuffer, NOT_FOUND_HEADERS, NOT_FOUND_BODY);
                    write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length); 
                    close(thread->connection);
                    continue;
                }
                
                struct dirent entry;
                struct dirent* entryp;

                int32_t dirCount = 0;
                int32_t fileCount = 0;

                readdir_r(dir, &entry, &entryp);
                while (entryp) {
                    if (array_equals(entry.d_name, 2, ".", 2) || array_equals(entry.d_name, 3, "..", 3)) {
                        readdir_r(dir, &entry, &entryp);
                        continue;
                    }

                    // Remove added entry name
                    thread->request.path.length = baseLength;
                    
                    buffer_appendFromString(&thread->request.path, entry.d_name);

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
                int32_t currentFile = 1;
                int32_t currentDir = 1;

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

                thread->request.path.length = baseLength;
                
                for (int32_t i = 0; i < dirCount; ++i) {
                    buffer_appendFromString(&thread->dirListingBuffer, "<li><a href=\"");
                    buffer_appendFromArray(&thread->dirListingBuffer, thread->request.path.data + 1, thread->request.path.length - 1); // Skip '.'
                    buffer_appendFromString(&thread->dirListingBuffer, directoryNames[i]);
                    buffer_appendFromString(&thread->dirListingBuffer, "/\">");
                    buffer_appendFromString(&thread->dirListingBuffer, directoryNames[i]);
                    buffer_appendFromString(&thread->dirListingBuffer, "/</a></li>\n");
                }

                for (int32_t i = 0; i < fileCount; ++i) {
                    buffer_appendFromString(&thread->dirListingBuffer, "<li><a href=\"");
                    buffer_appendFromArray(&thread->dirListingBuffer, thread->request.path.data + 1, thread->request.path.length - 1); // Skip '.'
                    buffer_appendFromString(&thread->dirListingBuffer, filenames[i]);
                    buffer_appendFromString(&thread->dirListingBuffer, "\">");
                    buffer_appendFromString(&thread->dirListingBuffer, filenames[i]);
                    buffer_appendFromString(&thread->dirListingBuffer, "</a></li>\n");
                }
                buffer_appendFromString(&thread->dirListingBuffer, "</ul></body></html>\n");
                
                buffer_appendFromString(&thread->responseBuffer, HTTP_OK_HEADER HTTP_CACHE_HEADERS "Content-Type: text/html" HTTP_NEWLINE);
                buffer_appendFromString(&thread->responseBuffer, HTTP_CONTENT_LENGTH_KEY);
                buffer_appendFromUint(&thread->responseBuffer, thread->dirListingBuffer.length);
                buffer_appendFromString(&thread->responseBuffer, HTTP_NEWLINE);
                buffer_appendFromString(&thread->responseBuffer, HTTP_DATE_KEY);
                buffer_appendDate(&thread->responseBuffer);
                buffer_appendFromString(&thread->responseBuffer, HTTP_END_HEADER);

                if (method == HTTP_METHOD_GET) {
                    buffer_appendFromArray(&thread->responseBuffer, thread->dirListingBuffer.data, thread->dirListingBuffer.length);
                }

                write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);  
                close(thread->connection);
                closedir(dir);
                continue;
            }
            
        }

        int32_t fd = buffer_openFile(&thread->request.path, O_RDONLY);

        if (fd == -1) {
            perror("Failed to open file");
            buffer_errorResponse(&thread->responseBuffer, NOT_FOUND_HEADERS, NOT_FOUND_BODY);
            write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);
            close(thread->connection);
            continue; 
        }

        returnVal = fstat(fd, &fileInfo);

        if (returnVal == -1) {
            perror("Failed to stat file");
            buffer_errorResponse(&thread->responseBuffer, NOT_FOUND_HEADERS, NOT_FOUND_BODY);
            write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);
            close(thread->connection);
            close(fd);
            continue;
        }

        int32_t fileSize = fileInfo.st_size;

        buffer_appendFromString(&thread->responseBuffer, HTTP_OK_HEADER);
        buffer_appendFromString(&thread->responseBuffer, HTTP_CACHE_HEADERS);
        buffer_appendFromString(&thread->responseBuffer, HTTP_CONTENT_TYPE_KEY);
        buffer_appendFromString(&thread->responseBuffer, contentTypeHeader(&thread->request.path));
        buffer_appendFromString(&thread->responseBuffer, HTTP_NEWLINE);
        buffer_appendFromString(&thread->responseBuffer, HTTP_CONTENT_LENGTH_KEY);
        buffer_appendFromUint(&thread->responseBuffer, fileSize);
        buffer_appendFromString(&thread->responseBuffer, HTTP_NEWLINE);
        buffer_appendFromString(&thread->responseBuffer, HTTP_DATE_KEY);
        buffer_appendDate(&thread->responseBuffer);
        buffer_appendFromString(&thread->responseBuffer, HTTP_END_HEADER);
        
        write(thread->connection, thread->responseBuffer.data, thread->responseBuffer.length);

        if (method == HTTP_METHOD_GET) {
            sendfile(thread->connection, fd, 0, fileSize);
        } 

        close(thread->connection);
        close(fd);
    }
}

// Close sockets, free memory, destroy thread
// control objects on process exit.
void onClose(void) {
    pthread_mutex_destroy(&currentConnectionLock);
    pthread_cond_destroy(&currentConnectionWritten);
    pthread_cond_destroy(&currentConnectionRead);

    if (!threads) {
        return;
    }

    for (int32_t i = 0; i < numThreads; ++i) {
        pthread_cancel(threads[i].thread);
        buffer_delete(&threads[i].requestBuffer);
        buffer_delete(&threads[i].responseBuffer);
        buffer_delete(&threads[i].request.method);
        buffer_delete(&threads[i].request.path);
        buffer_delete(&threads[i].request.version);
        buffer_delete(&threads[i].dirListingBuffer);
        buffer_delete(&threads[i].dirnameBuffer);
        buffer_delete(&threads[i].filenameBuffer);
        close(threads[i].connection);
    }
    free(threads);

    close(sock);
}

// Ensure cleanup happens when we get 
// signal that stops the process.
void onSignal(int sig) {
    exit(0);
}

int main(int argc, char** argv) {
    uint32_t port = 5000;

    numThreads = sysconf(_SC_NPROCESSORS_CONF);

    if (numThreads < 1) {
        numThreads = 1;
    }

    if (argc > 1) {
        uint32_t argPort = string_toUint(argv[1]);

        if (argPort > 0) {
            port = argPort;
        }
    }

    printf("Starting cervit v" VERSION " on port %d using %d threads\n", port, numThreads);

    atexit(onClose);
    signal(SIGINT, onSignal);
    signal(SIGQUIT, onSignal);
    signal(SIGABRT, onSignal);
    signal(SIGTSTP, onSignal);
    signal(SIGTERM, onSignal);

    int8_t initError = 0;

    // Set up thread control
    int32_t errorCode = 0;
    errorCode = pthread_mutex_init(&currentConnectionLock, NULL);
    if (errorCode) {
        fprintf(stderr, "Failed to create mutex. Error code: %d", errorCode);
        initError = 1;
    }

    errorCode = pthread_cond_init(&currentConnectionWritten, NULL);
    if (errorCode) {
        fprintf(stderr, "Failed to create write condition. Error code: %d", errorCode);
        initError = 1;
    }

    errorCode = pthread_cond_init(&currentConnectionRead, NULL);
    if (errorCode) {
        fprintf(stderr, "Failed to create read condition. Error code: %d", errorCode);
        initError = 1;
    }

    if (initError) {
        return 1;
    }

    //Initialize threads
    threads = malloc(numThreads * sizeof(Thread));

    if (!threads) {
        fprintf(stderr, "Failed to allocate thread array\n");
        return 1;
    }

    for (int32_t i = 0; i < numThreads; ++i) {
        threads[i].id = i;
        buffer_init(&threads[i].request.method, 16);
        buffer_init(&threads[i].request.path, 1024);
        buffer_init(&threads[i].request.version, 16);
        buffer_init(&threads[i].requestBuffer, 2048);
        buffer_init(&threads[i].responseBuffer, 1024);
        buffer_init(&threads[i].dirListingBuffer, 512);
        buffer_init(&threads[i].dirnameBuffer, 512);
        buffer_init(&threads[i].filenameBuffer, 512);
        errorCode = pthread_create(&threads[i].thread, NULL, handleRequest, &threads[i]);
        if (errorCode) {
            fprintf(stderr, "Failed to create thread. Error code: %d", errorCode);
            initError = 1;
        }
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("Failed to create socket");
        initError = 1;
    }

    if (initError) {
        return 1;
    }

    int32_t sockoptTrue = 1;
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
    int32_t connection;

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
