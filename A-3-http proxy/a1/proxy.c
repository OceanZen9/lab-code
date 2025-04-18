#include "proxy_parse.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/wait.h>
#include <netdb.h>
#include <unistd.h>
//进程数限制
#define MAX_PROC 100

//请求缓冲区大小
#define MAX_REQUEST_SIZE 8192
#define BUFFER_SIZA 8192
int active_proc = 0;

//信号处理函数声明
void handle_sigchld(int sig);

//客户端处理函数声明
void handle_client(int client_fd);

//客户端处理细节函数声明
int connect_to_server(const char *host, const char *port);
int forward_request(int server_fd, struct ParsedRequest *request);
int forward_response(int client_fd, int server_fd);
void send_error(int client_fd, int status_code, const char *message);



int main(int argc, char * argv[]) {
  //错误处理
  if (argc != 2) {
    fprintf(stderr, "%s [prot]\n", argv[0]);
    return 1;
  }

  //检测端口
  int port = atoi(argv[1]);
  if (port > 65535 || port < 0) {
    fprintf(stderr, "port %d out of range\n", port);
    return 1;
  }

  //创建socket
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket");
    return 1;
  }
  //设置socket选项
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));//SO_REUSEADDR允许重用本地地址和端口,在服务器关闭后，端口可以立即重用

  //准备地址结构
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons((uint16_t)port);

  //绑定socket
  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("bind");
    close(server_fd);
    return 1;
  }
  //监听socket
  if (listen(server_fd, 5) < 0) {
    perror("listen");
    close(server_fd);
    return 1;
  }

  //设置信号处理
  signal(SIGCHLD, handle_sigchld);
  
  while (1) {
    //接受连接
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
      perror("accept");
      close(server_fd);
      continue;
    }
    
    //检查进程数
    if (active_proc >= MAX_PROC) {
      fprintf(stderr, "out of range %d\n", MAX_PROC);
      close(client_fd);
      continue;
    }

    //创建子进程
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      close(client_fd);
      continue;
    }
    if (pid > 0) {
      //父进程
      close(client_fd);
      active_proc++;
      continue;
    } else {
      //子进程
      close(server_fd);
      handle_client(client_fd);
      close(client_fd);
      exit(0);
    }
  }
  

  return 0;
}

void handle_sigchld(int sig) {
  int saved_errno = errno;

  //回收所有以终止子进程
  while (waitpid(-1, NULL, WNOHANG) > 0) {
    active_proc--;
  }

  errno = saved_errno;
}

void handle_client(int client_fd) {
  char buffer[MAX_REQUEST_SIZE];
  //读取请求
  int bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
  if (bytes_read < 0) {
    perror("read");
    return;
  }
  buffer[bytes_read] = '\0';

  //解析请求
  struct ParsedRequest *request = ParsedRequest_create();
  if (ParsedRequest_parse(request, buffer, bytes_read) < 0) {
    fprintf(stderr, "parse failed\n");
    ParsedRequest_destroy(request);
    return;
  }

  //检查请求方法
  if (strcmp(request->method, "GET") != 0) {
    fprintf(stderr, "unsupported method: %s\n", request->method);
    ParsedRequest_destroy(request);
    return;
  }

  //连接到目标服务器
  int server_fd = connect_to_server(request->host, request->port);
  if (server_fd < 0) {
    fprintf(stderr, "connect to server failed\n");
    ParsedRequest_destroy(request);
    return;
  }
  //发送请求到目标服务器
  if (forward_request(server_fd, request) < 0) {
    close(server_fd);
    send_error(client_fd, 500, "Internal Server Error");
    ParsedRequest_destroy(request);
    return;
  }
  //接收响应
  if (forward_response(client_fd, server_fd) < 0) {
    fprintf(stderr, "forward response failed\n");
    close(server_fd);
    ParsedRequest_destroy(request);
    return;
  }
  //清理资源
  close(server_fd);
  ParsedRequest_destroy(request);
}

void send_error(int client_fd, int status_code, const char *message) {
  char response[1024];
  snprintf(response,sizeof(response),
            "HTTP/1.0 %d %s\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "<html><body><h1>%d %s</h1></body></html>\r\n",
            status_code, message, status_code, message);
  send(client_fd, response, strlen(response), 0);
}

int connect_to_server(const char *host, const char *port) {
  struct hostent *server;
  struct sockaddr_in server_addr;
  int server_fd;

  //创建socket
  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket");
    return -1;
  }
  server = gethostbyname(host);
  if (server == NULL) {
    fprintf(stderr, "gethostbyname failed\n");
    close(server_fd);
    return -1;
  }
  
  //设置服务器地址
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
  server_addr.sin_port = htons(atoi(port));

  //连接到服务器
  if (connect(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("connect");
    close(server_fd);
    return -1;
  }
  return server_fd;
}

int forward_request(int server_fd, struct ParsedRequest *request) {
  //构建新的请求行
  char request_line[1024];
  snprintf(request_line, sizeof(request_line), "GET %s HTTP/1.0\r\n", request->path);

  //发送请求行
  if (send(server_fd, request_line, strlen(request_line), 0) < 0) {
    perror("send");
    return -1;
  }

  //发送请求头
  char headers_buffer[MAX_REQUEST_SIZE];
  if (ParsedRequest_unparse_headers(request, headers_buffer, sizeof(headers_buffer)) < 0) {
    fprintf(stderr, "unparse headers failed\n");
    return -1;
  }

  //发送头部
  if (send(server_fd, headers_buffer, strlen(headers_buffer), 0) < 0) {
    perror("send");
    return -1;
  }
  return 0;
}

int forward_response(int client_fd, int server_fd) {
  char buffer[BUFFER_SIZA];
  ssize_t bytes_read;

  //读取并转发服务器响应
  while ((bytes_read = recv(server_fd, buffer, sizeof(buffer), 0)) > 0) {
    if (send(client_fd, buffer, bytes_read, 0) < 0) {
      perror("send");
      return -1;
    }
  }

  if (bytes_read < 0) {
    perror("recv");
    return -1;
  }

  return 0;
}
