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
#define BUFFER_SIZE 8192
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
  //SO_REUSEADDR允许重用本地地址和端口,在服务器关闭后，端口可以立即重用
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt");
    close(server_fd);
    return 1;
  }

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
  struct sigaction sa;
  sa.sa_handler = handle_sigchld;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if (sigaction(SIGCHLD, &sa, NULL) < 0) {
    perror("sigaction");
    close(server_fd);
    return 1;
  }
  
  
  while (1) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    //接受连接
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue; //被信号中断，继续接受连接
      }
      perror("accept");
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
  pid_t pid;
  int status;

  //回收所有以终止子进程
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    active_proc--;
  }

  errno = saved_errno;
}

int read_request(int client_fd, char *buffer, int buffer_size) {
  int total_bytes = 0;
  int bytes_read;

  while(1) {
    bytes_read = read(client_fd, buffer + total_bytes, buffer_size - total_bytes - 1);
    if (bytes_read <= 0) {
      return bytes_read;
    }

    total_bytes += bytes_read;
    buffer[total_bytes] = '\0'; //c语言字符串本质为字符数组，必须以'\0'结尾

    //检查是否收到完整http请求头
    if (strstr(buffer, "\r\n\r\n") != NULL) {
      break;
    }

    //检查缓冲区是否已满
    if (total_bytes >= buffer_size - 1) {
      fprintf(stderr, "out of buffer_size\n");
      return -1;
    }
  }
  return total_bytes;
}

void handle_client(int client_fd) {
  char buffer[MAX_REQUEST_SIZE];
  //读取请求
  int bytes_read = read_request(client_fd, buffer, sizeof(buffer));
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
    send_error(client_fd, 501, "Not Implemented");
    ParsedRequest_destroy(request);
    return;
  }

  const char *server_port = request->port ? request->port : "80";
  //连接到目标服务器
  int server_fd = connect_to_server(request->host, server_port);
  if (server_fd < 0) {
    fprintf(stderr, "connect to server failed\n");
    send_error(client_fd, 502, "Bad Gateway");
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
  char body[512];

  //构建响应体
  snprintf(body, sizeof(body), "<html><body><h1>%d %s</h1></body></html>", status_code, message);
  snprintf(response, sizeof(response),
          "HTTP/1.0 %d %s\r\n"
          "Content-Type: text/html\r\n"
          "Content-Length: %zu\r\n"
          "Connection: close\r\n"
          "\r\n"
          "%s",
          status_code, message, strlen(body), body);
  send(client_fd, response, strlen(response), 0);
}

int connect_to_server(const char *host, const char *port) {
  struct addrinfo hints, *servinfo, *p;
  int server_fd, rv;
  
  //设置服务器地址
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  //获取地址信息，DNS解析
  if ((rv = getaddrinfo(host, port, &hints, &servinfo)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    return -1;
  }
  //遍历所有可能地址，尝试连接
  for (p = servinfo; p != NULL; p = p->ai_next) {
    if ((server_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) {
      perror("socket");
      continue;
    }

    if (connect(server_fd, p->ai_addr, p->ai_addrlen) < 0) {
      perror("connect");
      close(server_fd);
      continue;
    }
    break;
  }

  if (p == NULL) {
    fprintf(stderr, "fail to fetch server %s:%s\n", host, port);
    freeaddrinfo(servinfo);
    return -1;
  }
  freeaddrinfo(servinfo);
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

  //确保有Host头
  struct ParsedHeader *host_header = ParsedHeader_get(request, "Host");
  if (!host_header) {
    char host_buf[1024];
    snprintf(host_buf, sizeof(host_buf), "Host: %s\r\n", request->host);
    if (send(server_fd, host_buf, strlen(host_buf), 0) < 0) {
      perror("failed to send Host_head");
      return -1;
    }
  }

  //确保Connection： close头
  struct ParsedHeader *conn_header = ParsedHeader_get(request, "Connection");
  if (!conn_header) {
    char conn_buf[1024];
    snprintf(conn_buf, sizeof(conn_buf), "Connection: close\r\n");
    if (send(server_fd, conn_buf, strlen(conn_buf), 0) < 0) {
      perror("failed to send conn_head");
      return -1;
    }
  }
  //将解析后的头部转换为字符串，存入 headers_buffer
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
  char buffer[BUFFER_SIZE];
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
