#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>

uint16_t compute_checksum(uint16_t *addr, int len);//计算ICMP报文的校验和
void send_ping(int socket_fd, struct sockaddr_in *addr, int seq);//构造和发送ICMP Echo请求
int recv_ping(int socket_fd, int seq, struct timeval *tv_send, double *rtt);//接受和处理ICMP Echo回复

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <IP address>\n", argv[0]);
        return 1;
    }

    char *ip_address = argv[1];
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip_address, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP address: %s\n", ip_address);
        return 1;
    }

    int socket_fd;
    if ((socket_fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) < 0) {
        perror("socket");
        return 1;
    }

    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout) < 0) {
        perror("setsockopt");
        close(socket_fd);
        return 1;
    }

    struct timeval tv_send;
    int count = 0;                  // ping 次数计数器
    int num_packets = 5;
    double min_rtt = 1000000.0;     // 初始化最小 RTT 为一个很大的值
    double max_rtt = 0.0;           // 初始化最大 RTT 为 0
    double sum_rtt = 0.0;           // RTT 总和
    double sum_rtt_squared = 0.0;   // RTT 平方和，用于计算标准差
    double rtt;

    while (count < num_packets) { // 循环发送 ping 请求
        double rtt_current = 0; // 当前 RTT
        send_ping(socket_fd, &addr, count);
        gettimeofday(&tv_send, NULL);
        if (recv_ping(socket_fd, count, &tv_send, &rtt_current) == 0) { // 接收 ping 回复
            rtt = rtt_current;
            count++;
            min_rtt = (rtt < min_rtt) ? rtt : min_rtt; // 更新最小 RTT
            max_rtt = (rtt > max_rtt) ? rtt : max_rtt; // 更新最大 RTT
            sum_rtt += rtt;                           // 累加 RTT
            sum_rtt_squared += rtt * rtt;             // 累加 RTT 的平方
            count ++;
        } else {
            printf("Request timed out\n"); // 超时处理
        }

        sleep(1); // 暂停 1 秒

        // 打印统计信息
        if (count > 0) {
            double avg_rtt = sum_rtt / count;
            double std_dev = sqrt((sum_rtt_squared / count) - (avg_rtt * avg_rtt)); // 计算标准差
            printf("--- %s ping statistics ---\n", inet_ntoa(addr.sin_addr));
            printf("%d packets transmitted, %d received, %.2f%% packet loss, time %.0fms\n", count, count, 0.0, sum_rtt); 
            printf("rtt min/avg/max/mdev = %.2f/%.2f/%.2f/%.2f ms\n", min_rtt, avg_rtt, max_rtt, std_dev);
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

    // 将数据按16位分组求和
    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    // 如果数据长度为奇数，处理最后一个字节
    if (nleft == 1) {
        *(unsigned char *)(&answer) = *(unsigned char *)w;
        sum += answer;
    }

    // 将高16位和低16位相加
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);

    // 取反
    answer = ~sum;
    return answer;
}

void send_ping(int socket_fd, struct sockaddr_in *addr, int seq) {
    struct icmp icmp_hdr;
    memset(&icmp_hdr, 0, sizeof(icmp_hdr));
    icmp_hdr.icmp_type = ICMP_ECHO;;
    icmp_hdr.icmp_code = 0;
    icmp_hdr.icmp_cksum = 0;
    icmp_hdr.icmp_seq = seq;
    icmp_hdr.icmp_id = getpid();
    gettimeofday((struct timeval *)&icmp_hdr.icmp_data, NULL);
    icmp_hdr.icmp_cksum = compute_checksum((uint16_t *)&icmp_hdr, sizeof(icmp_hdr));
    if (sendto(socket_fd, &icmp_hdr, sizeof(icmp_hdr), 0, (struct sockaddr *)addr, sizeof(*addr)) <= 0) {
        perror("sendto");
    }
    printf("Sent ICMP Echo request to %s\n", inet_ntoa(addr->sin_addr));
}

int recv_ping(int socket_fd, int seq, struct timeval *tv_send, double *rtt) {
    char buffer[1024];
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    struct icmp *icmp_hdr;
    struct timeval tv_recv;
    memset(buffer, 0, sizeof(buffer));
    if (recvfrom(socket_fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&addr, &addr_len) <= 0) {
        perror("recvfrom");
        return 1;
    }
    gettimeofday(&tv_recv, NULL);
    icmp_hdr = (struct icmp *)(buffer + sizeof(struct ip));
    if (icmp_hdr->icmp_type == ICMP_ECHOREPLY && icmp_hdr->icmp_seq == seq) {
        *rtt = (tv_recv.tv_sec - tv_send->tv_sec) * 1000.0 + (tv_recv.tv_usec - tv_send->tv_usec) / 1000.0; // 计算 RTT 并通过指针返回
        printf("Received ICMP Echo reply from %s: seq=%d time=%.2f ms\n", inet_ntoa(addr.sin_addr), seq, *rtt);
    } else {
        printf("Received unexpected ICMP packet\n");
    }
    return 0;
}