#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>

uint16_t compute_checksum(uint16_t *addr, int len);//计算ICMP报文的校验和
void send_ping(int socket_fd, struct sockaddr_in *addr, int seq);//构造和发送ICMP Echo请求
int recv_ping(int socket_fd, int seq, struct timeval *tv_send);//接受和处理ICMP Echo回复

int main(int argv, char *argc[]) {
    if (argv != 2) {
        fprintf(stderr, "Usage: %s <IP address>\n", argc[0]);
    }
    char *ip_address = argc[1];
    //创建socket
    int socket_fd;
    if (socket_fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP) < 0) {
        perror("socket");
        return 1;
    }
    //设置接受超时
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout)) {
        perror("setsockopt");
        close(socket_fd);
        return 1;
    }
    //构造ICMP报文结构
    struct icmp *icmp_hdr;
    memset(&icmp_hdr, 0, sizeof(icmp_hdr));
    icmp_hdr->icmp_type = ICMP_ECHO;
    icmp_hdr->icmp_code = 0;
    icmp_hdr->icmp_cksum = 0;
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

int recv_ping(int socket_fd, int seq, struct timeval *tv_send) {
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
        double rtt = (tv_recv.tv_sec - tv_send->tv_sec) * 1000.0 + (tv_recv.tv_usec - tv_send->tv_usec) / 1000.0;
        printf("Received ICMP Echo reply from %s: seq=%d time=%.2f ms\n", inet_ntoa(addr.sin_addr), seq, rtt);
    } else {
        printf("Received unexpected ICMP packet\n");
    }
    close(socket_fd);
    return 0;
}