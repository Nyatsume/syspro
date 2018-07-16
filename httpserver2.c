#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>

#define SERVER_NAME "sysproHTTP"
#define SERVER_VERSION "1.0"
#define HTTP_MINOR_VERSION 0
#define BLOCK_BUF_SIZE 1024
#define LINE_BUF_SIZE 4096
#define MAX_REQUEST_BODY_LENGTH (1024 *1024)
#define TIME_BUF_SIZE 64
struct HTTPheader {
  char *name;
  char *value;
  struct HTTPheader *next;
};
struct HTTPreq {
  int protocol_minor_version;
  char *method;
  char *path;
  struct HTTPheader *header;
  char *body;
  long length;
};

struct FileInfo {
  char *path;
  long size;
  int ok;
};

typedef void (*sighandler_t)(int);
static void sigpipe(void);
static void trap_signal(int sig, sighandler_t handler);
static void signal_exit(int sig);
static void service(FILE *in, FILE *out, char *docroot);
static struct HTTPreq* readreq(FILE *in);
static void readreq_line(struct HTTPreq *req, FILE *in);
static struct HTTPheader* read_header(FILE *in);
static void upcase(char *str);
static void freereq(struct HTTPreq *req);
static long content_length(struct HTTPreq *req);
static char* lookup_header(struct HTTPreq *req, char *name);
static void respond_to(struct HTTPreq *req, FILE *out, char *docroot);
static void file_response(struct HTTPreq *req, FILE *out, char *docroot);
static void method_not_allowed(struct HTTPreq *req, FILE *out);
static void not_implemented(struct HTTPreq *req, FILE *out);
static void not_found(struct HTTPreq *req,FILE *out);
static void output_common_header(struct HTTPreq *req, FILE *out, char *status);
static struct FileInfo* get_fileinfo(char *docroot, char *path);
static char* build_filepath(char *docroot, char *path);
static void free_fileinfo(struct FileInfo *info);
static char* guess_content_type(struct FileInfo *info);
static void* manage_memory(size_t sz);
static void err_log(char *fmt, ...);


static void err_log(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}
static void* manage_memory(size_t sz) {
  void *p;

  p = malloc(sz);
  if (!p) err_log("メモリ割り当て失敗");
  return p;
}

static void sigpipe(void) {
  trap_signal(SIGPIPE, signal_exit);
}

static void trap_signal(int sig, sighandler_t handler) {
  struct sigaction act;

  act.sa_handler = handler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = SA_RESTART;
  if (sigaction(sig, &act, NULL) < 0)
    err_log("sigaction() failed: %s", strerror(errno));
}

static void signal_exit(int sig) {
  err_log("シグナル %d により中止されました", sig);
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <docroot>\n", argv[0]);
    exit(1);
  }

  sigpipe();
  service(stdin, stdout, argv[1]);
  exit(0);
}


static void service(FILE *in, FILE *out, char *docroot) {
  struct HTTPreq *req;

  req = readreq(in);
  respond_to(req, out, docroot);
  freereq(req);
}
static struct HTTPreq* readreq(FILE *in) {
  struct HTTPreq *req;
  struct HTTPheader *h;

  req = manage_memory(sizeof(struct HTTPreq));
  readreq_line(req, in);
  req->header = NULL;
  while (h = read_header(in)) {
    h->next = req->header;
    req->header = h;
  }
  req->length = content_length(req);
  if (req->length != 0) {
    if (req->length > MAX_REQUEST_BODY_LENGTH)
      err_log("エンティティボディのサイズが大きすぎます!");
    req->body = manage_memory(req->length);
    if (fread(req->body, req->length, 1, in) <1)
      err_log("エンティティボディの読み込みに失敗しました。");
  }else {
    req->body = NULL;
  }
  return req;
}

static void freereq(struct HTTPreq *req) {
  struct HTTPheader *h, *head;

  head = req->header;
  while (head) {
    h = head;
    head = head-> next;
    free(h->name);
    free(h->value);
    free(h);
  }
  free(req->method);
  free(req->path);
  free(req->body);
  free(req);
}

static void readreq_line(struct HTTPreq *req, FILE *in) {
  char buf[LINE_BUF_SIZE];
  char *path, *p;

  if (!fgets(buf, LINE_BUF_SIZE, in))
    err_log("リクエストラインがありません");
  p = strchr(buf, ' ');
  if (!p) 
    err_log("構文解析エラー リクエストライン(1): %s", buf);
  *p++ = '\0';
  req->method = manage_memory(p - buf);
  strcpy(req->method, buf);
  upcase(req->method);

  path = p;
  p = strchr(path, ' ');
  if (!p)
    err_log("構文解析エラー リクエストライン(2): %s", buf);
  *p++ = '\0';
  req->path = manage_memory(p - path);
  strcpy(req->path, path);

  if (strncasecmp(p, "HTTP/1.", strlen("HTTP/1.")) != 0)
    err_log("構文解析エラー リクエストライン(3): %s", buf);
  p += strlen("HTTP/1.");
  req->protocol_minor_version = atoi(p);
}

static struct HTTPheader* read_header(FILE *in) {
  struct HTTPheader *h;
  char buf[LINE_BUF_SIZE];
  char *p;

  if (!fgets(buf, LINE_BUF_SIZE, in))
    err_log("リクエストヘッダーの読み込みに失敗しました: %s", strerror(errno));
  if ((buf[0] == '\n') || (strcmp(buf, "\r\n") == 0))
    return NULL;

  p = strchr(buf, ':');
  if (!p) err_log("構文解析エラー リクエストヘッダー: %s", buf);
  *p++ = '\0';
  h = manage_memory(sizeof(struct HTTPheader));
  h->name = manage_memory(p - buf);
  strcpy(h->name, buf);

  p += strspn(p, " \t");
  h->value = manage_memory(strlen(p) + 1);
  strcpy(h->value, p);

  return h;
}

static void upcase(char *str) {
  char *p;

  for (p = str; *p; p++) {
    *p = (char)toupper((int)*p);
  }
}

static long content_length(struct HTTPreq *req) {
  char *val;
  long len;

  val = lookup_header(req, "Content-Length");
  if (!val) 
    return 0;
  len = atoi(val);
  if (len < 0)
    err_log("negative Content-length value");
    return len;
}

static char* lookup_header(struct HTTPreq *req, char *name) {
  struct HTTPheader *h;

  for (h = req->header; h; h = h->next) {
    if (strcasecmp(h->name, name) == 0)
      return h->value;
  }
  return NULL;
}

static struct FileInfo* get_fileinfo(char *docroot, char *urlpath) {
  struct FileInfo *info;
  struct stat st;

  info = manage_memory(sizeof(struct FileInfo));
  info->path = build_filepath(docroot, urlpath);
  info->ok = 0;
  if (lstat(info->path, &st) < 0)
    return info;
  if (!S_ISREG(st.st_mode))
    return info;
  info->ok = 1;
  info->size = st.st_size;
  return info;
}
static char * build_filepath(char *docroot, char *urlpath) {
  char *path;

  path = manage_memory(strlen(docroot) + 1 + strlen(urlpath) + 1);
  sprintf(path, "%s/%s", docroot, urlpath);
  return path;
}

static void respond_to(struct HTTPreq *req, FILE *out, char *docroot) {
  if (strcmp(req->method, "GET") == 0)
    file_response(req, out, docroot);
  else if (strcmp(req->method, "HEAD") == 0)
    file_response(req, out, docroot);
  else if (strcmp(req->method, "POST") == 0)
    method_not_allowed(req, out);
  else
    not_implemented(req, out);
}


static void file_response(struct HTTPreq *req, FILE *out, char *docroot) {
  struct FileInfo *info;

  info = get_fileinfo(docroot, req->path);
  if (!info->ok) {
     free_fileinfo(info);
     not_found(req,out);
     return;
  }
  output_common_header(req, out, "200 OK");
  fprintf(out, "Content-Length: %ld\r\n", info->size);
  fprintf(out, "Content-Type: %s\r\n", guess_content_type(info));
  fprintf(out, "\r\n");
  if (strcmp(req->method, "HEAD") != 0) {
    int fd;
    char buf[BLOCK_BUF_SIZE];
    ssize_t n;

    fd = open(info->path, O_RDONLY);
    if (fd < 0)
      err_log("%sの展開に失敗しました: %s", info->path, strerror(errno));
    while (1) {
      n = read(fd, buf, BLOCK_BUF_SIZE);
      if (n < 0)
        err_log("%sの展開に失敗しました:%s", info->path, strerror(errno));
      if (n == 0)
        break;
      if (fwrite(buf, 1, n, out) < n)
        err_log("ソケットへの書き込みに失敗しました");
    }
    close(fd);
  }
  fflush(out);
  free_fileinfo(info);
}

static void output_common_header(struct HTTPreq *req, FILE *out, char *status) {
  time_t t;
  struct tm *tm;
  char buf[TIME_BUF_SIZE];

  t = time(NULL);
  tm = gmtime(&t);
  if (!tm)
    err_log("gmtime() failed: %s", strerror(errno));
  strftime(buf, TIME_BUF_SIZE, "%a, %d %b %Y %H:%M:%S GMT", tm);
  fprintf(out, "HTTP/1.%d %s\r\n", HTTP_MINOR_VERSION, status);
  fprintf(out, "Date: %s\r\n", buf);
  fprintf(out, "Server: %s/%s\r\n", SERVER_NAME, SERVER_VERSION);
  fprintf(out, "Connection: close\r\n");
}

static void method_not_allowed(struct HTTPreq *req, FILE *out) {
  output_common_header(req, out, "405 Method Not Allowed");
  fprintf(out, "Content-Type: text/html\r\n");
  fprintf(out, "\r\n");
  fprintf(out, "<html>\r\n");
  fprintf(out, "<header>\r\n");
  fprintf(out, "<title>405 Method Not Allowed</title>\r\n");
  fprintf(out, "<header>\r\n");
  fprintf(out, "<body>\r\n");
  fprintf(out, "<p>The request method %s is not allowed</p>\r\n", req->method);
  fprintf(out, "</body>\r\n");
  fprintf(out, "</html>\r\n");
  fflush(out);
}

static void not_implemented(struct HTTPreq *req, FILE *out) {
  output_common_header(req, out, "501 Not Implemented");
  fprintf(out, "Content-type: text/html\r\n");
  fprintf(out,"\r\n");
  fprintf(out,"<html>\r\n");
  fprintf(out,"<header>\r\n");
  fprintf(out,"<title>501 Not Implemeted</title>\r\n");
  fprintf(out,"<header>\r\n");
  fprintf(out,"<body>\r\n");
  fprintf(out,"<p>The request method %s is not implemented</>\r\n", req->method);
  fprintf(out,"</body>\r\n");
  fprintf(out,"</html>\r\n");
  fflush(out);
}

static void not_found(struct HTTPreq *req, FILE *out) {
  output_common_header(req, out, "404 Not Found");
  fprintf(out, "Content-Type: text/html\r\n");
  fprintf(out, "\r\n");
  if (strcmp(req->method, "HEAD") != 0) {
    fprintf(out, "<html>\r\n");
    fprintf(out, "<header><title>Not Found</title></header>\r\n");
    fprintf(out, "<body><p>File not found</p></body>\r\n");
    fprintf(out, "</html>\r\n");
  }
  fflush(out);
}

static void free_fileinfo(struct FileInfo *info) {
  free(info->path);
  free(info);
}

static char* guess_content_type(struct FileInfo *info) {
  return "text/plain";
}
