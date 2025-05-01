#include <stdio.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>
#include <errno.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>

// 确保在 macOS 上定义 ICMP_TIME_EXCEEDED
#ifndef ICMP_TIME_EXCEEDED
#define ICMP_TIME_EXCEEDED 11
#endif

// 调试宏
#define DEBUG 1
#define DEBUG_PRINT(fmt, ...) \
    do { if (DEBUG) fprintf(stderr, "[DEBUG] " fmt, ##__VA_ARGS__); } while (0)

// 函数声明
uint16_t compute_checksum(uint16_t *addr, int len);
void send_probe(int socket_fd, struct sockaddr_in *addr, int seq, int ttl);
int recv_response(int socket_fd, int seq, struct timeval *tv_send, double *rtt, struct sockaddr_in *from_addr, int *icmp_type);

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "使用方法: %s <IP地址>\n", argv[0]);
        return 1;
    }

    char *ip_address = argv[1];
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip_address, &addr.sin_addr) <= 0) {
        fprintf(stderr, "无效的IP地址: %s\n", ip_address);
        return 1;
    }

    // 创建原始套接字 - 关键修改：不设置 IP_HDRINCL
    int socket_fd;
    if ((socket_fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) < 0) {
        perror("创建套接字失败");
        fprintf(stderr, "请确保您以root权限运行此程序 (sudo)\n");
        return 1;
    }
    
    // 设置接收超时
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout) < 0) {
        perror("设置套接字超时选项失败");
        close(socket_fd);
        return 1;
    }

    printf("traceroute 到 %s (%s), 最大跳数 %d\n", ip_address, ip_address, 30);

    struct timeval tv_send;
    struct sockaddr_in from_addr;
    double rtt;
    int icmp_type;
    int seq = 0;
    int max_ttl = 30;
    int max_tries = 3;
    int reached_dest = 0;

    for (int ttl = 1; ttl <= max_ttl && !reached_dest; ttl++) {
        printf("%2d  ", ttl);
        fflush(stdout);

        // 设置TTL值
        if (setsockopt(socket_fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
            perror("设置TTL失败");
            close(socket_fd);
            return 1;
        }

        int success = 0;
        for (int tries = 0; tries < max_tries && !success; tries++) {
            // 发送探测包
            send_probe(socket_fd, &addr, seq, ttl);
            gettimeofday(&tv_send, NULL);
            
            DEBUG_PRINT("已发送探测包: TTL=%d, seq=%d\n", ttl, seq);
            
            // 等待并接收回复
            usleep(10000);  // 10ms
            int ret = recv_response(socket_fd, seq, &tv_send, &rtt, &from_addr, &icmp_type);

            if (ret == 0) {
                success = 1;
                printf("%s  %.2f ms", inet_ntoa(from_addr.sin_addr), rtt);

                if (icmp_type == ICMP_ECHOREPLY) {
                    reached_dest = 1;
                }
            } else if (tries == max_tries - 1) {
                printf("*");
            }
            
            seq++;
            if (!success && tries < max_tries - 1) {
                usleep(100000); // 100ms
            }
        }
        printf("\n");
        
        if (reached_dest) {
            printf("已到达目标 %s\n", inet_ntoa(from_addr.sin_addr));
        }
    }
    
    close(socket_fd);
    return 0;
}

uint16_t compute_checksum(uint16_t *addr, int len) {
    int nleft = len;
    int sum = 0;
    uint16_t *w = addr;
    uint16_t answer = 0;

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    if (nleft == 1) {
        *(unsigned char *)(&answer) = *(unsigned char *)w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    answer = ~sum;
    return answer;
}

// 修改后的 send_probe 函数 - 解决 "Invalid argument" 问题
void send_probe(int socket_fd, struct sockaddr_in *addr, int seq, int ttl) {
    // 创建ICMP数据包
    char packet[64]; // 足够大的缓冲区
    struct icmp *icmp = (struct icmp *)packet;
    
    // 清空缓冲区
    memset(packet, 0, sizeof(packet));
    
    // 填充ICMP头部
    icmp->icmp_type = ICMP_ECHO;
    icmp->icmp_code = 0;
    icmp->icmp_id = htons(getpid() & 0xFFFF); // 网络字节序
    icmp->icmp_seq = htons(seq);              // 网络字节序
    
    // 获取当前时间作为ICMP负载
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    // 在macOS上，ICMP数据必须至少8字节
    memcpy(packet + 8, &tv, sizeof(tv));
    
    // 设置数据包大小 (头部8字节+时间戳)
    int packet_size = 8 + sizeof(tv);
    
    // 计算校验和
    icmp->icmp_cksum = 0;
    icmp->icmp_cksum = compute_checksum((uint16_t *)icmp, packet_size);
    
    // 发送数据包
    int sent = sendto(socket_fd, packet, packet_size, 0,
                     (struct sockaddr *)addr, sizeof(*addr));
    
    if (sent <= 0) {
        DEBUG_PRINT("发送探测包失败: %s\n", strerror(errno));
    } else {
        DEBUG_PRINT("成功发送 %d 字节的ICMP探测包\n", sent);
    }
}

int recv_response(int socket_fd, int seq, struct timeval *tv_send, double *rtt, struct sockaddr_in *from_addr, int *icmp_type) {
    char buffer[1024];
    socklen_t addr_len = sizeof(*from_addr);
    struct ip *ip_hdr;
    struct icmp *icmp_hdr;
    struct timeval tv_recv;

    memset(buffer, 0, sizeof(buffer));

    int recv_len = recvfrom(socket_fd, buffer, sizeof(buffer), 0, 
                           (struct sockaddr *)from_addr, &addr_len);
    if (recv_len <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            DEBUG_PRINT("接收超时 (seq=%d)\n", seq);
            return 1;
        } else {
            DEBUG_PRINT("接收数据包失败: %s\n", strerror(errno));
            return -1;
        }
    }

    gettimeofday(&tv_recv, NULL);
    DEBUG_PRINT("接收到数据包，长度: %d 字节\n", recv_len);

    // 解析IP头部
    ip_hdr = (struct ip *)buffer;
    int ip_hdr_len = ip_hdr->ip_hl * 4;
    
    if (ip_hdr_len + 8 > recv_len) {
        DEBUG_PRINT("接收到的数据包太短\n");
        return 1;
    }
    
    // 获取ICMP头部
    icmp_hdr = (struct icmp *)(buffer + ip_hdr_len);
    *icmp_type = icmp_hdr->icmp_type;
    DEBUG_PRINT("ICMP类型: %d\n", *icmp_type);

    // 处理ICMP消息
    if (*icmp_type == ICMP_TIME_EXCEEDED) {
        DEBUG_PRINT("收到TTL超时消息\n");
        
        // 提取原始IP和ICMP头部
        if (ip_hdr_len + 8 + sizeof(struct ip) > recv_len) {
            DEBUG_PRINT("数据包太短，无法获取原始IP头部\n");
            return 1;
        }
        
        struct ip *orig_ip = (struct ip *)(buffer + ip_hdr_len + 8);
        int orig_ip_len = orig_ip->ip_hl * 4;
        
        if ((char *)orig_ip + orig_ip_len + 8 > buffer + recv_len) {
            DEBUG_PRINT("数据包太短，无法获取原始ICMP头部\n");
            return 1;
        }
        
        struct icmp *orig_icmp = (struct icmp *)((char *)orig_ip + orig_ip_len);
        
        // 检查原始ICMP数据包ID是否匹配
        uint16_t recv_id = ntohs(orig_icmp->icmp_id);
        uint16_t our_id = getpid() & 0xFFFF;
        
        DEBUG_PRINT("原始ICMP - ID:%u (我们的ID:%u), 类型:%d, 序列:%u\n", 
                   recv_id, our_id, orig_icmp->icmp_type, ntohs(orig_icmp->icmp_seq));
        
        if (orig_icmp->icmp_type == ICMP_ECHO && recv_id == our_id) {
            // 获取时间戳
            if ((char *)orig_icmp + 8 + sizeof(struct timeval) > buffer + recv_len) {
                DEBUG_PRINT("数据包太短，无法获取原始时间戳\n");
                return 1;
            }
            
            struct timeval tv_orig;
            memcpy(&tv_orig, (char *)orig_icmp + 8, sizeof(struct timeval));
            
            *rtt = (tv_recv.tv_sec - tv_orig.tv_sec) * 1000.0 + 
                   (tv_recv.tv_usec - tv_orig.tv_usec) / 1000.0;
                   
            DEBUG_PRINT("成功解析TTL超时信息, RTT=%.2f ms\n", *rtt);
            return 0;
        }
    } else if (*icmp_type == ICMP_ECHOREPLY) {
        DEBUG_PRINT("收到Echo回复\n");
        
        // 检查ID是否匹配
        uint16_t recv_id = ntohs(icmp_hdr->icmp_id);
        uint16_t our_id = getpid() & 0xFFFF;
        
        DEBUG_PRINT("Echo回复 - ID:%u (我们的ID:%u), 序列:%u\n", 
                   recv_id, our_id, ntohs(icmp_hdr->icmp_seq));
        
        if (recv_id == our_id) {
            // 获取时间戳
            if ((char *)icmp_hdr + 8 + sizeof(struct timeval) > buffer + recv_len) {
                DEBUG_PRINT("数据包太短，无法获取时间戳\n");
                return 1;
            }
            
            struct timeval tv_orig;
            memcpy(&tv_orig, (char *)icmp_hdr + 8, sizeof(struct timeval));
            
            *rtt = (tv_recv.tv_sec - tv_orig.tv_sec) * 1000.0 + 
                   (tv_recv.tv_usec - tv_orig.tv_usec) / 1000.0;
                   
            DEBUG_PRINT("成功解析Echo回复, RTT=%.2f ms\n", *rtt);
            return 0;
        }
    }
    
    return 1;
}