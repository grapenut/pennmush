/**
 * \file http.h
 *
 * \brief Utilities for serving HTTP requests.
 */

#ifndef _HTTP_H
#define _HTTP_H

#include "copyrite.h"

/* request states */
#define HTTP_REQUEST_HEADERS		1
#define HTTP_REQUEST_CONTENT		2
#define HTTP_REQUEST_DONE		3
#define HTTP_REQUEST_STARTED		4

/* content headers */
#define HTTP_CONTENT_LENGTH		"Content-Length"
#define HTTP_CONTENT_TYPE		"Content-Type"

/* connection timeout in seconds */
#define HTTP_TIMEOUT			2

/* the public prototypes */
extern bool is_http_request(char *command);
extern int process_http_request(DESC *d, char *command, int got);
extern int do_http_command(DESC *d, char *command);

extern const char *http_method_str[];

COMMAND_PROTO(cmd_respond);

#endif /* _HTTP_H */

