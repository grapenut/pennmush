
#include <string.h>
#include <ctype.h>

#include "mushtype.h"
#include "strutil.h"
#include "game.h"
#include "externs.h"
#include "attrib.h"
#include "mymalloc.h"
#include "case.h"
#include "command.h"
#include "parse.h"

#include "http.h"

static const char *get_http_status(uint32_t code);

static http_method parse_http_method(char *command);
static int parse_http_query(http_request *req, char *line);
static void parse_http_header(http_request *req, char *line);
static int parse_http_content(http_request *req, char *line);

static int process_http_helper(DESC *d, char *command);

static bool run_http_request(DESC *d);

static void reset_http_timeout(DESC *d, int time);
bool http_timeout_wrapper(void *data);

static void send_http_status(DESC *d, uint32_t status, char *content);
static void send_mudurl(DESC *d);
static void send_http_response(DESC *d, char *content);
static void close_http_request(DESC *d);

/* from bsd.c */
extern int queue_write(DESC *d, const char *b, int n);
extern int queue_eol(DESC *d);

const char *http_method_str[] = {
  "UNKNOWN ",
  "GET ",
  "POST ",
  "PUT ",
  "PATCH ",
  "DELETE ",
  NULL
};

/* HTTP status code, e.g. "200 Ok", "404 Not Found"
 * Indexed by integer code, with string value
 */
struct HTTP_STATUS_CODE {
  uint32_t code;
  const char *str;
};

extern struct HTTP_STATUS_CODE http_status_codes[];

/** Parse the HTTP method from a command string
 * \param command string to parse
 */
static http_method
parse_http_method(char *command)
{
  http_method i;

  for (i = HTTP_METHOD_GET; i < HTTP_NUM_METHODS; i++) {
    if (!strncmp(command, http_method_str[i], strlen(http_method_str[i]))) {
      return i;
    }
  }
  return HTTP_METHOD_UNKNOWN;
}

/** Return the status code string.
 * \param code the status code to transform
 */
static const char *
get_http_status(uint32_t code)
{
  struct HTTP_STATUS_CODE *c;
  
  for (c = http_status_codes; c && c->code; c++)
  {
    if (c->code == code) {
      return c->str;
    }
  }
  
  return NULL;
}

/** Test for a HTTP request command
 * \param command string to test
 */
bool
is_http_request(char *command)
{
  http_method i = parse_http_method(command);
  
  if (i != HTTP_METHOD_UNKNOWN) {
    return true;
  }
  
  return false;
}

/** Parse the HTTP request query string
 * \param req http request object
 * \param line query string to process
 */
static int
parse_http_query(http_request *req, char *line)
{
  char *c, *path, *query, *version;
  http_method method;

  /* Parse a line of the format:
   * METHOD /route/path?query_string HTTP/1.1
   */
  
  if (!req) {
    return 0;
  }
  
  /* extract the method from the start of line */
  method = parse_http_method(line);
  if (method == HTTP_METHOD_UNKNOWN) {
    return 0;
  }
  req->method = method;
  
  /* skip ahead to the path */
  c = strchr(line, ' ');
  if (!c) {
    return 0;
  }
  *(c++) = '\0';

  /* skip extra spaces and get the path+query string */
  for (path = c; *path && isspace(*path); path++);
  c = strchr(path, ' ');
  if (!c) {
    return 0;
  }
  *(c++) = '\0';
  
  /* make sure the path isn't too long */
  if (strlen(path) >= HTTP_STR_LEN) {
    return 0;
  }
  
  /* check the version string, why not? */
  version = c;
  if (strncmp(version, "HTTP/1.1", 8)) {
    return 0;
  }
  
  /* find the optional query string */
  c = strchr(path, '?');
  if (c) {
    *(c++) = '\0';
    query = c;
    strncpy(req->query, query, HTTP_STR_LEN - 1);
  }
  
  /* copy the path, with query string removed */
  strncpy(req->path, path, HTTP_STR_LEN - 1);
  
  /* initialize the request metadata */
  req->state = HTTP_REQUEST_HEADERS;
  req->timer = NULL;
  req->length = 0;
  req->recv = 0;
  req->hp = req->headers;
  req->cp = req->content;
  req->rp = req->response;
  
  /* Default HTTP response metadata */
  req->status = 200;
  strncpy(req->res_type, "Content-Type: text/plain\r\n", HTTP_STR_LEN);
  
  /* setup the route attribute, skip leading slashes */
  for (c = path; *c == '/'; c++);
  if (*c) {
    /* path has more to it that just /, let's parse the rest */
    path = c;
  
    /* and trailing slashes */
    for (c = path; *c; c++);
    for (c--; c >= path && *c == '/'; c--);
    c++;
    if (*c == '/') {
      *c = '\0';
    }
  
    /* swap slashes / for ticks ` */
    for (c = path; *c; c++) {
      if (*c == '/') {
        *c = '`';
      }
    }
  
    /* convert the string to upper case */
    c = path;
    while (*c)
    {
      *c = UPCASE(*c);
      c++;
    }
  } else {
    /* the path was just /, default to INDEX */
    path = "INDEX";
  }
  
  /* copy the route attribute name */
  snprintf(req->route, HTTP_STR_LEN, "HTTP`%s", path);

  return 1;
}

/** Parse an HTTP request header string
 * \param req http request object
 * \param line header string to process
 */
static void
parse_http_header(http_request *req, char *line)
{
  char *value;
  size_t len = strlen(line);
  safe_strl(line, len, req->headers, &(req->hp));
  safe_chr('\n', req->headers, &(req->hp));
  *(req->hp) = '\0';
  
  value = strchr(line, ':');
  if (!value) {
    return;
  }
  *(value++) = '\0';
  
  if (!strncmp(line, HTTP_CONTENT_LENGTH, strlen(HTTP_CONTENT_LENGTH))) {
    req->length = parse_integer(value);
  } else if (!strncmp(line, HTTP_CONTENT_TYPE, strlen(HTTP_CONTENT_TYPE))) {
    strncpy(req->type, value, strlen(value));
  }
}

/** Parse an HTTP request content string
 * \param req http request object
 * \param line content string to process
 * \retval 1 received content-length number of bytes, done processing
 * \retval 0 continue processing
 */
static int
parse_http_content(http_request *req, char *line)
{
  size_t len = strlen(line);
  
  safe_strl(line, len, req->content, &(req->cp));
  *(req->cp) = '\0';
  
  req->recv += len;
  
  if (req->recv >= req->length) {
    return 1;
  }
  
  return 0;
}


/** Process HTTP request headers and data
 * \param d descriptor of http request
 * \param command command string to buffer
 * \param got number of bytes to read from command
 */
int
process_http_request(DESC *d, char *command, int got)
{
  char cbuf[MAX_COMMAND_LEN];
  char *q, *head, *end;
  
  /* local copy of the command buffer */
  strncpy(cbuf, command, got);
  
  /* split multi-line input and process one line at a time */
  head = cbuf;
  for (q = cbuf, end = cbuf + got; q < end; q++) {
    if (*q == '\r' || *q == '\n') {
      *q = '\0';
      if (!process_http_helper(d, head)) {
        return 0;
      }
      
      if (((q + 1) < end) && (*(q + 1) == '\n')) {
        q++;
      }
      head = q+1;
    }
  }
  
  /* handle single line, or the last line of multi-line */
  if (q > head) {
    *q = '\0';
    if (!process_http_helper(d, head)) {
      return 0;
    }
  }

  /* setup a timer to end the connection if no
   * more data is sent with a few seconds
   */
  reset_http_timeout(d, HTTP_TIMEOUT);
  return 1;
}

/** Process one line of the HTTP request
 * \param d descriptor of http request
 * \param command string to parse
 */
int
process_http_helper(DESC *d, char *command)
{
  char buff[BUFFER_LEN];
  char *bp;
  int done;
  http_request *req = d->http;
  
  if (!req || !command) {
    send_mudurl(d);
    return 0;
  }

  if (req->state == HTTP_REQUEST_HEADERS) {
    /* a blank line ends the headers */
    if (*command == '\0') {
      if (req->method == HTTP_METHOD_GET) {
        req->state = HTTP_REQUEST_DONE;
        /* we don't need to parse any content, just call the route event */
        if (!run_http_request(d)) {
          bp = buff;
          safe_format(buff, &bp, "File not found. \"%s\"", req->route);
          *bp = '\0';
          send_http_status(d, 404, buff);
          return 0;
        }
      } else {
        req->state = HTTP_REQUEST_CONTENT;
      }
    } else {
      parse_http_header(req, command);
    }
  } else if (req->state == HTTP_REQUEST_CONTENT) {
    done = parse_http_content(req, command);
    if (done) {
      req->state = HTTP_REQUEST_DONE;
      /* we finished parsing content, call the route event */
      if (!run_http_request(d)) {
        bp = buff;
        safe_format(buff, &bp, "File not found. \"%s\"", req->route);
        *bp = '\0';
        send_http_status(d, 404, buff);
        return 0;
      }
    }
  }

  return 1;
}

/** Parse an HTTP request 
 * \param d descriptor of http request
 * \param command command string to parse
 */
int
do_http_command(DESC *d, char *command)
{
  http_request *req;
  ATTR *a;
  
  /* if the route handler doesn't exist we can
   * close the connection early without parsing
   */
  a = atr_get_noparent(EVENT_HANDLER, "HTTP");
  if (!a) {
    send_mudurl(d);
    return 0;
  }

  /* allocate the http_request to hold headers and path info */
  req = d->http = mush_malloc(sizeof(http_request), "http_request");
  if (!req) {
    send_http_status(d, 500, "Unable to allocate http request.");
    return 0;
  }
  memset(req, 0, sizeof(http_request));
  
  /* parse the query string, return 400 if request is bad */
  if (!parse_http_query(d->http, command)) {
    send_http_status(d, 400, "Invalid request method.");
    return 0;
  }
  
  /* setup a timer to end the connection if no
   * more data is sent with a few seconds
   */
  reset_http_timeout(d, HTTP_TIMEOUT);
  return 1;
}

/** Queue the HTTP request on the event queue.
 * \param d descriptor of the http request
 */
static bool
run_http_request(DESC *d)
{
  http_request *req = d->http;
  
  if (!req) {
    return false;
  }

  return queue_event(EVENT_HANDLER, req->route,
                     "%d,%s,%s,%s,%s,%s,%d,%s,%s",
                     d->descriptor,
                     d->ip,
                     http_method_str[req->method],
                     req->path,
                     req->query,
                     req->type,
                     req->length,
                     req->headers,
                     req->content);
}

/** Reset the HTTP timeout
 * \param d descript of the http request
 * \param time the timeout length in cycles
 */
static void
reset_http_timeout(DESC *d, int time)
{
  http_request *req = d->http;

  if (!req) {
    return;
  }
  
  if (req->timer) {
    sq_cancel(req->timer);
    req->timer = NULL;
  }
  
  req->timer = sq_register_in(time, http_timeout_wrapper, (void *) d, NULL);
}

/** HTTP connection timeout wrapper
 * \param data descriptor of the http request
 */
bool
http_timeout_wrapper(void *data)
{
  DESC *d = (DESC *) data;
  http_request *req;
  
  if (!d) {
    return false;
  }
  
  req = d->http;
  if (!req) {
    return false;
  }

  /* we didn't finish parsing content, but call the route event anyway */
  if (req->state < HTTP_REQUEST_DONE) {
    req->state = HTTP_REQUEST_DONE;
    if (!run_http_request(d)) {
      send_http_status(d, 404, "File not found.");
      close_http_request(d);
      return false;
    }
    
    /* we have a slow connection that already timed out once
     * but we have enough info to start executing the request
     * let's reset the timeout on a short fuse */
    reset_http_timeout(d, 1);
    return false;
  }
  
  /* send a timeout message if we haven't already started a response */
  if (req->state != HTTP_REQUEST_STARTED) {
    send_http_status(d, 408, "Unable to complete request.");
  }
  
  /* we made it all the way through to here, guess we should shutdown the socket */
  close_http_request(d);
  return false;
}

/** Send a HTTP response code.
 * \param d descriptor of the http request
 */  
static void
send_http_status(DESC *d, uint32_t status, char *content)
{
  char buff[BUFFER_LEN];
  char *bp = buff;
  const char *code;
  
  http_request *req = d->http;
  
  if (!req) {
    return;
  }
  
  code = get_http_status(status);
  if (!code) {
    return;
  }
  
  safe_format(buff, &bp,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: text/html; charset:iso-8859-1\r\n"
              "Pragma: no-cache\r\n"
              "Connection: Close\r\n"
              "X-Route: %s\r\n"
              "\r\n"
              "<!DOCTYPE html>\r\n"
              "<HTML><HEAD>"
              "<TITLE>%d %s</TITLE>"
              "</HEAD><BODY><p>%s</p>\r\n"
              "</BODY></HTML>\r\n",
              status, code, req->route, status, code, content);
  *bp = '\0';
  
  queue_write(d, buff, bp - buff);
  queue_eol(d);
}

/** Send the MUDURL default webpage
 * \param d descriptor of the http request
 */  
static void
send_mudurl(DESC *d)
{
  char buff[BUFFER_LEN];
  char *bp = buff;
  bool has_mudurl = strncmp(MUDURL, "http", 4) == 0;

  safe_format(buff, &bp,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/html; charset:iso-8859-1\r\n"
              "Pragma: no-cache\r\n"
              "Connection: Close\r\n"
              "\r\n"
              "<!DOCTYPE html>\r\n"
              "<HTML><HEAD>"
              "<TITLE>Welcome to %s!</TITLE>",
              MUDNAME);
  if (has_mudurl) {
    safe_format(buff, &bp,
                "<meta http-equiv=\"refresh\" content=\"5; url=%s\">",
                MUDURL);
  }
  safe_str("</HEAD><BODY><h1>Oops!</h1>", buff, &bp);
  if (has_mudurl) {
    safe_format(buff, &bp,
                "<p>You've come here by accident! Please click <a "
                "href=\"%s\">%s</a> to go to the website for %s if your "
                "browser doesn't redirect you in a few seconds.</p>",
                MUDURL, MUDURL, MUDNAME);
  } else {
    safe_format(buff, &bp,
                "<p>You've come here by accident! Try using a MUSH client, "
                "not a browser, to connect to %s.</p>",
                MUDNAME);
  }
  safe_str("</BODY></HTML>\r\n", buff, &bp);
  *bp = '\0';
  queue_write(d, buff, bp - buff);
  queue_eol(d);
}

static void
send_http_response(DESC *d, char *content)
{
  char buff[BUFFER_LEN];
  char *bp;
  const char *status;
  http_request *req = d->http;
  
  if (!req) {
    return;
  }
  
  /* build the response buffer */
  bp = buff;
  
  /* only send the headers the first call */
  if (req->state != HTTP_REQUEST_STARTED) {
    req->state = HTTP_REQUEST_STARTED;
    status = get_http_status(req->status);
    safe_format(buff, &bp, "HTTP/1.1 %u %s\r\n", req->status, status);
    safe_str(req->response, buff, &bp);
    safe_str(req->res_type, buff, &bp);
    //safe_format(buff, &bp, "Content-Length: %lu\r\n", strlen(arg_right));
    safe_str("\r\n", buff, &bp);
    
    if (req->wrap_html && strstr(req->res_type, "text/html")) {
      safe_format(buff, &bp,
                  "<!DOCTYPE html>\r\n"
                  "<HTML><HEAD>\r\n"
                  "<TITLE>%s</TITLE>\r\n"
                  "</HEAD><BODY>\r\n",
                  MUDNAME);
    }
  }
  
  /* response content, if present */
  if (content && *content) {
    safe_str(content, buff, &bp);
  } 
    
  *bp = '\0';
  
  /* send the response */
  queue_write(d, buff, bp - buff);
  queue_eol(d);
}

/** Close the HTTP request socket and clean up timers
 * \param d decriptor of the http request
 */
static void
close_http_request(DESC *d)
{
  char buff[BUFFER_LEN];
  char *bp;
  http_request *req = d->http;
  
  if (req) {
    if (req->timer) {
      sq_cancel(req->timer);
      req->timer = NULL;
    }
    
    /* send the closing HTML wrapper if needed */
    if (req->wrap_html && strstr(req->res_type, "text/html")) {
      bp = buff;
      safe_str("</BODY></HTML>\r\n", buff, &bp);
      *bp = '\0';

      queue_write(d, buff, bp - buff);
      queue_eol(d);
    }
  }
  
  boot_desc(d, "http close", GOD);
}

/* @respond command used to send HTTP responses */
COMMAND(cmd_respond)
{
  const char *p;
  http_request *req;
  uint32_t code;
  DESC *d;
  int arg_content = 1;
  int close_socket = 1;
  
  if (!arg_left || !*arg_left) {
    notify(executor, T("Invalid arguments."));
    return;
  }
  
  d = port_desc(parse_integer(arg_left));
  if (!d) {
    notify(executor, T("Descriptor not found."));
    return;
  }
  
  if (!d->http || !(d->conn_flags & CONN_HTTP_REQUEST)) {
    notify(executor, T("Descriptor has not made an HTTP request."));
    return;
  }
  
  /* reset the timeout since we set some data */
  reset_http_timeout(d, HTTP_TIMEOUT);
    
  req = d->http;
  
  /* if /html, /text, or /json are set change the Content-Type */
  if (SW_ISSET(sw, SWITCH_HTML)) {
    strncpy(req->res_type, "Content-Type: text/html\r\n", HTTP_STR_LEN);
    notify(executor, T("Content-Type set to text/html."));
    close_socket = 0;
  } else if (SW_ISSET(sw, SWITCH_JSON)) {
    strncpy(req->res_type, "Content-Type: application/json\r\n", HTTP_STR_LEN);
    notify(executor, T("Content-Type set to application/json."));
    close_socket = 0;
  } else if (SW_ISSET(sw, SWITCH_TEXT)) {
    strncpy(req->res_type, "Content-Type: text/plain\r\n", HTTP_STR_LEN);
    notify(executor, T("Content-Type set to text/plain."));
    close_socket = 0;
  }
  
  /* using a type switch by itself, e.g. @respond/html %0, should
   * not close the socket, unless you provide content in arg_right.
   * can still be blocked with /send, or forced with /notify.
   */
  if (arg_right && *arg_right) {
    close_socket = 1;
  }

  /* toggle whether to wrap the output with HTML boilerplate */
  if ((SW_ISSET(sw, SWITCH_WRAP))) {
    req->wrap_html = true;
  } else if ((SW_ISSET(sw, SWITCH_NOWRAP))) {
    req->wrap_html = false;
  }
  
  if (SW_ISSET(sw, SWITCH_TYPE)) {
    /* @respond/type set the content-type header, defaults to text/plain */
    
    if (!arg_right || !*arg_right) {
      notify(executor, T("Invalid arguments."));
      return;
    }
    
    snprintf(req->res_type, HTTP_STR_LEN, "Content-Type: %s\r\n", arg_right);
    
    notify_format(executor, T("Content-Type set to %s."), arg_right);
    return;
  
  } else if (SW_ISSET(sw, SWITCH_HEADER)) {
    /* @respond/header set any other headers */
    
    if (!arg_right || !*arg_right) {
      notify(executor, T("Invalid arguments."));
      return;
    }
    
    /* check the header format */
    p = strchr(arg_right, ':');
    if (!p || (p - arg_right) < 1) {
      notify(executor, T("Invalid format, expected \"Header-Name: Value\"."));
      return;
    }
    
    /* prevent hijacking Content-Type or Content-Length */
    if (!strncasecmp(arg_right, HTTP_CONTENT_LENGTH, strlen(HTTP_CONTENT_LENGTH))) {
      notify(executor, T("You may not manually set the Content-Length header."));
      return;
    } else if (!strncasecmp(arg_right, HTTP_CONTENT_TYPE, strlen(HTTP_CONTENT_TYPE))) {
      notify(executor, T("You may not manually set the Content-Type header."));
      return;
    }
    
    /* save the response header */
    safe_str(arg_right, req->response, &(req->rp));
    safe_str("\r\n", req->response, &(req->rp));
    *(req->hp) = '\0';

    notify_format(executor, T("Header added, %s."), arg_right);

    /* return here, unless we need to /notify and disconnect */
    if (!(SW_ISSET(sw, SWITCH_NOTIFY))) {
      return;
    }
    
    /* arg_right was already used, don't send it as content */
    arg_content = 0;
    
  } else if (SW_ISSET(sw, SWITCH_STATUS)) {
    /* @respond/status set the response status code, default 200 Ok */
    
    if (!arg_right || !*arg_right) {
      notify(executor, T("Invalid arguments."));
      return;
    }
    
    code = parse_uint32(arg_right, NULL, 10);
    p = get_http_status(code);
    if (!p) {
      notify(executor, T("Invalid HTTP status code."));
      return;
    }
    
    req->status = code;

    notify_format(executor, T("Status code set to %u %s."), code, p);
    
    /* return here, unless we need to /notify and disconnect */
    if (!(SW_ISSET(sw, SWITCH_NOTIFY))) {
      return;
    }

    /* arg_right was already used, don't send it as content */
    arg_content = 0;
    
  }
  
  /* none of the sub-commands exitted early so send a response
   * check arg_content to make sure we didn't already use arg_right
   */
  if (arg_content) {
    send_http_response(d, arg_right);
  } else {
    send_http_response(d, NULL);
  }
    
  /* close the socket unless /send is set, unless /notify overrides that */
  if (close_socket && ((SW_ISSET(sw, SWITCH_NOTIFY)) || !(SW_ISSET(sw, SWITCH_SEND)))) {
    close_http_request(d);
  }

  return;
}


/* define the status codes here so that they don't clutter
 * the head of the file.
 */
struct HTTP_STATUS_CODE http_status_codes[] = {
  {100, "Continue"},
  {101, "Switching Protocols"},
  {102, "Processing"},
  {103, "Early Hints"},
  {200, "OK"},
  {201, "Created"},
  {202, "Accepted"},
  {203, "Non-Authoritative Information"},
  {204, "No Content"},
  {205, "Reset Content"},
  {206, "Partial Content"},
  {207, "Multi-Status"},
  {208, "Already Reported"},
  {226, "IM Used"},
  {300, "Multiple Choices"},
  {301, "Moved Permanently"},
  {302, "Found"},
  {303, "See Other"},
  {304, "Not Modified"},
  {305, "Use Proxy"},
  {306, "(Unused)"},
  {307, "Temporary Redirect"},
  {308, "Permanent Redirect"},
  {400, "Bad Request"},
  {401, "Unauthorized"},
  {402, "Payment Required"},
  {403, "Forbidden"},
  {404, "Not Found"},
  {405, "Method Not Allowed"},
  {406, "Not Acceptable"},
  {407, "Proxy Authentication Required"},
  {408, "Request Timeout"},
  {409, "Conflict"},
  {410, "Gone"},
  {411, "Length Required"},
  {412, "Precondition Failed"},
  {413, "Payload Too Large"},
  {414, "URI Too Long"},
  {415, "Unsupported Media Type"},
  {416, "Range Not Satisfiable"},
  {417, "Expectation Failed"},
  {421, "Misdirected Request"},
  {422, "Unprocessable Entity"},
  {423, "Locked"},
  {424, "Failed Dependency"},
  {425, "Too Early"},
  {426, "Upgrade Required"},
  {428, "Precondition Required"},
  {429, "Too Many Requests"},
  {431, "Request Header Fields Too Large"},
  {451, "Unavailable For Legal Reasons"},
  {500, "Internal Server Error"},
  {501, "Not Implemented"},
  {502, "Bad Gateway"},
  {503, "Service Unavailable"},
  {504, "Gateway Timeout"},
  {505, "HTTP Version Not Supported"},
  {506, "Variant Also Negotiates"},
  {507, "Insufficient Storage"},
  {508, "Loop Detected"},
  {510, "Not Extended"},
  {511, "Network Authentication Required"}
};




