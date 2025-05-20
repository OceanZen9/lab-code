#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/ip_icmp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <math.h>    
#include <errno.h>   
#include <signal.h>

uint16_t compute_checksum(uint16_t *addr, int len);//计算ICMP报文的校验和
void send_ping(int socket_fd, struct sockaddr_in *addr, int seq);//构造和发送ICMP Echo请求
int recv_ping(int socket_fd, int seq, struct timeval *tv_send, double *rtt);//接受和处理ICMP Echo回复

volatile sig_atomic_t stop_ping = 0;
int total_packets_sent_final = 0;
int total_packets_received_final = 0;
double total_sum_rtt_final = 0.0;
double total_sum_rtt_squared_final = 0.0;
double total_min_rtt_final = 1000000.0;
double total_max_rtt_final = 0.0;
struct timeval tv_start_total_final, tv_end_total_final;

void sigint_handler(int signum) {
    (void)signum; 
    stop_ping = 1;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ip_address>\n", argv[0]);
        return 1;
    }

    char *ip_address = argv[1];
    int socket_fd;
    struct sockaddr_in addr;
    struct timeval timeout;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip_address, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid IP address: %s\n", ip_address);
        return 1;
    }

    socket_fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (socket_fd < 0) {
        perror("socket");
        return 1;
    }

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout) < 0) {
        perror("setsockopt");
        close(socket_fd);
        return 1;
    }

    // 注册信号处理函数
    signal(SIGINT, sigint_handler);

    //初始化ping相关变量
    int packets_to_send = 5;
    int packets_sent = 0;
    int packets_received = 0;

    //RTT相关统计变量
    double rtt, min_rtt = 1000000.0, max_rtt = 0.0, sum_rtt = 0.0, sum_rtt_squared = 0.0;
    double avg_rtt = 0.0, std_dev = 0.0;
    double rtt_current;
    struct timeval tv_send, tv_start_run, tv_end_run;

    printf("PING %s (%s)\n", ip_address, ip_address); // 类似标准ping的起始输出

    gettimeofday(&tv_start_run, NULL); // 记录整个ping过程的开始时间
    gettimeofday(&tv_start_total_final, NULL);

    for (int i = 0; i < packets_to_send && !stop_ping; ++i) {
        send_ping(socket_fd, &addr, i); // 使用循环变量 i 作为序列号
        packets_sent++;
        total_packets_sent_final = packets_sent;

        gettimeofday(&tv_send, NULL); // 精确记录发送时间

        // recv_ping 函数内部会处理打印接收信息或超时信息
        if (recv_ping(socket_fd, i, &tv_send, &rtt_current) == 0) { // 检查序列号 i
            rtt = rtt_current;
            packets_received++;
            total_packets_received_final = packets_received; // Update for final stats

            if (rtt < min_rtt) min_rtt = rtt;
            if (rtt > max_rtt) max_rtt = rtt;
            sum_rtt += rtt;
            sum_rtt_squared += rtt * rtt;

            if (rtt < total_min_rtt_final) total_min_rtt_final = rtt;
            if (rtt > total_max_rtt_final) total_max_rtt_final = rtt;
            total_sum_rtt_final = sum_rtt;
            total_sum_rtt_squared_final = sum_rtt_squared;
        }
        if (!stop_ping && i < packets_to_send - 1) { // 如果不是最后一个包且未被中断
             sleep(1); // 发送间隔
        }
    }

    gettimeofday(&tv_end_run, NULL); // 记录整个ping过程的结束时间
    gettimeofday(&tv_end_total_final, NULL);

    // --- 最终统计信息 ---
    if (stop_ping) {
        packets_sent = total_packets_sent_final;
        packets_received = total_packets_received_final;
        sum_rtt = total_sum_rtt_final;
        sum_rtt_squared = total_sum_rtt_squared_final;
        min_rtt = total_min_rtt_final;
        max_rtt = total_max_rtt_final;
        tv_start_run = tv_start_total_final;
        tv_end_run = tv_end_total_final;
    }


    printf("\n--- %s ping statistics ---\n", ip_address);
    double loss_rate = 0.0;
    if (packets_sent > 0) {
        loss_rate = ((double)(packets_sent - packets_received) / packets_sent) * 100.0;
    }
    
    double total_elapsed_time_ms = (tv_end_run.tv_sec - tv_start_run.tv_sec) * 1000.0 +
                                 (tv_end_run.tv_usec - tv_start_run.tv_usec) / 1000.0;

    printf("%d packets transmitted, %d received, %.2f%% packet loss, time %.0fms\n",
           packets_sent, packets_received, loss_rate, total_elapsed_time_ms);

    if (packets_received > 0) {
        avg_rtt = sum_rtt / packets_received;
         if (packets_received > 1) {
           double variance = (sum_rtt_squared / packets_received) - (avg_rtt * avg_rtt);
            std_dev = (variance > 0) ? sqrt(variance) : 0.0;
        } else {
            std_dev = 0.0;
        }
        // 确保min_rtt没有因为未收到包而保持初始值
        if (packets_received == 0) {
             min_rtt = 0.0;
        }

        printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms\n",
               min_rtt == 1000000.0 ? 0.0 : min_rtt,
               avg_rtt,
               max_rtt,
               std_dev);
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