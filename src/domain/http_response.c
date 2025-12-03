/**
 * @file http_response.c
 * @brief Implementation of HTTP response generation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../include/domain/http_response.h"
#include "../../third_party/libreactor/src/reactor.h"

#define _GNU_SOURCE
#define _GNU_SOURCE
// #include <dynamic.h>
#include <reactor.h>

/** Thread-local date header buffer */
static __thread char date_header[38] = "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n";

static data http_date(time_t t)
{
    static __thread char date_buffer[30];
    time_t now = t ? t : time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    size_t len = strftime(date_buffer, sizeof(date_buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return data_construct(date_buffer, len);
}

/** Content type strings */
static const char *content_type_strings[] = {
    [CONTENT_TYPE_TEXT_PLAIN] = "text/plain",
    [CONTENT_TYPE_APPLICATION_JSON] = "application/json"
};

/** Status line strings */
static const char *status_strings[] = {
    [HTTP_STATUS_OK] = "HTTP/1.1 200 OK\r\n",
    [HTTP_STATUS_NOT_FOUND] = "HTTP/1.1 404 Not Found\r\n",
    [HTTP_STATUS_INTERNAL_ERROR] = "HTTP/1.1 500 Internal Server Error\r\n"
};

http_response_error_t http_response_init(void)
{
    /* Currently no initialization needed */
    return HTTP_RESPONSE_OK;
}

void http_response_cleanup(void)
{
    /* Currently no cleanup needed */
}

size_t http_response_calculate_size(const http_response_config_t *config)
{
    if (!config) {
        return 0;
    }

    size_t size = 0;

    /* Status line */
    const char *status_str = http_response_status_string(config->status_code);
    if (!status_str) {
        return 0;
    }
    size += strlen(status_str);

    /* Server header */
    size += strlen("Server: L\r\n");

    /* Date header (if requested) */
    if (config->include_date_header) {
        size += sizeof(date_header) - 1; /* -1 for null terminator */
    }

    /* Content-Type header */
    size += strlen("Content-Type: ");
    const char *content_type_str = http_response_content_type_string(config->content_type);
    if (!content_type_str) {
        return 0;
    }
    size += strlen(content_type_str);
    size += strlen("\r\n");

    /* Content-Length header */
    size += strlen("Content-Length: ");
    /* Max content length is 10 digits for uint32_t max */
    size += 10;
    size += strlen("\r\n");

    /* Header separator */
    size += strlen("\r\n");

    /* Body */
    size += config->body_length;

    return size;
}

http_response_error_t http_response_build(http_response_buffer_t *buffer,
                                          const http_response_config_t *config)
{
    if (!buffer || !config) {
        return HTTP_RESPONSE_ERROR_INVALID_PARAM;
    }

    if (buffer->used > 0) {
        return HTTP_RESPONSE_ERROR_INVALID_PARAM; /* Buffer not reset */
    }

    char *ptr = buffer->buffer;
    size_t remaining = buffer->size;

    /* Status line */
    const char *status_str = http_response_status_string(config->status_code);
    if (!status_str) {
        return HTTP_RESPONSE_ERROR_INVALID_PARAM;
    }
    size_t status_len = strlen(status_str);
    if (status_len >= remaining) {
        return HTTP_RESPONSE_ERROR_BUFFER_OVERFLOW;
    }
    memcpy(ptr, status_str, status_len);
    ptr += status_len;
    remaining -= status_len;

    /* Server header */
    const char *server_str = "Server: L\r\n";
    size_t server_len = strlen(server_str);
    if (server_len >= remaining) {
        return HTTP_RESPONSE_ERROR_BUFFER_OVERFLOW;
    }
    memcpy(ptr, server_str, server_len);
    ptr += server_len;
    remaining -= server_len;

    /* Date header (if requested) */
    if (config->include_date_header) {
        data date_seg = http_date(0);
        memcpy(date_header + 6, date_seg.base, date_seg.size);
        date_header[6 + date_seg.size] = '\r';
        date_header[6 + date_seg.size + 1] = '\n';
        date_header[6 + date_seg.size + 2] = '\0';

        size_t date_len = strlen(date_header);
        if (date_len >= remaining) {
            return HTTP_RESPONSE_ERROR_BUFFER_OVERFLOW;
        }
        memcpy(ptr, date_header, date_len);
        ptr += date_len;
        remaining -= date_len;
    }

    /* Content-Type header */
    const char *content_type_str = http_response_content_type_string(config->content_type);
    if (!content_type_str) {
        return HTTP_RESPONSE_ERROR_INVALID_PARAM;
    }

    const char *content_type_prefix = "Content-Type: ";
    size_t content_type_prefix_len = strlen(content_type_prefix);
    size_t content_type_len = strlen(content_type_str);
    const char *content_type_suffix = "\r\n";

    if (content_type_prefix_len + content_type_len + 2 >= remaining) {
        return HTTP_RESPONSE_ERROR_BUFFER_OVERFLOW;
    }

    memcpy(ptr, content_type_prefix, content_type_prefix_len);
    ptr += content_type_prefix_len;
    remaining -= content_type_prefix_len;

    memcpy(ptr, content_type_str, content_type_len);
    ptr += content_type_len;
    remaining -= content_type_len;

    memcpy(ptr, content_type_suffix, 2);
    ptr += 2;
    remaining -= 2;

    /* Content-Length header */
    const char *content_length_prefix = "Content-Length: ";
    size_t content_length_prefix_len = strlen(content_length_prefix);

    /* Convert body length to string */
    char length_str[16];
    int length_str_len = snprintf(length_str, sizeof(length_str), "%zu", config->body_length);
    if (length_str_len < 0 || (size_t)length_str_len >= sizeof(length_str)) {
        return HTTP_RESPONSE_ERROR_INVALID_PARAM;
    }

    const char *content_length_suffix = "\r\n";

    if (content_length_prefix_len + length_str_len + 2 >= remaining) {
        return HTTP_RESPONSE_ERROR_BUFFER_OVERFLOW;
    }

    memcpy(ptr, content_length_prefix, content_length_prefix_len);
    ptr += content_length_prefix_len;
    remaining -= content_length_prefix_len;

    memcpy(ptr, length_str, length_str_len);
    ptr += length_str_len;
    remaining -= length_str_len;

    memcpy(ptr, content_length_suffix, 2);
    ptr += 2;
    remaining -= 2;

    /* Header separator */
    if (2 >= remaining) {
        return HTTP_RESPONSE_ERROR_BUFFER_OVERFLOW;
    }
    memcpy(ptr, "\r\n", 2);
    ptr += 2;
    remaining -= 2;

    /* Body */
    if (config->body && config->body_length > 0) {
        if (config->body_length >= remaining) {
            return HTTP_RESPONSE_ERROR_BUFFER_OVERFLOW;
        }
        memcpy(ptr, config->body, config->body_length);
        ptr += config->body_length;
        remaining -= config->body_length;
    }

    buffer->used = buffer->size - remaining;
    return HTTP_RESPONSE_OK;
}

http_response_error_t http_response_buffer_init(http_response_buffer_t *buffer,
                                                char *buffer_ptr,
                                                size_t buffer_size)
{
    if (!buffer || !buffer_ptr || buffer_size == 0) {
        return HTTP_RESPONSE_ERROR_INVALID_PARAM;
    }

    buffer->buffer = buffer_ptr;
    buffer->size = buffer_size;
    buffer->used = 0;

    return HTTP_RESPONSE_OK;
}

void http_response_buffer_reset(http_response_buffer_t *buffer)
{
    if (buffer) {
        buffer->used = 0;
    }
}

const char *http_response_content_type_string(content_type_t type)
{
    if (type >= sizeof(content_type_strings) / sizeof(content_type_strings[0])) {
        return NULL;
    }
    return content_type_strings[type];
}

const char *http_response_status_string(http_status_t status)
{
    switch (status) {
        case HTTP_STATUS_OK:
            return status_strings[HTTP_STATUS_OK];
        case HTTP_STATUS_NOT_FOUND:
            return status_strings[HTTP_STATUS_NOT_FOUND];
        case HTTP_STATUS_INTERNAL_ERROR:
            return status_strings[HTTP_STATUS_INTERNAL_ERROR];
        default:
            return NULL;
    }
}
