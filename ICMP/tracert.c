#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>

// 确保在 macOS 或其他可能未定义此宏的系统上定义 ICMP_TIME_EXCEEDED
#ifndef ICMP_TIME_EXCEEDED
#define ICMP_TIME_EXCEEDED 11 // ICMP Time Exceeded code
#endif

// 调试宏 - 设置为 0 来关闭调试输出，获取干净的traceroute结果
#define DEBUG 0
#define DEBUG_PRINT(fmt, ...) \
    do { if (DEBUG) fprintf(stderr, "[DEBUG] " fmt, ##__VA_ARGS__); } while (0)

// 函数声明
uint16_t compute_checksum(uint16_t *addr, int len);
void send_probe(int socket_fd, const struct sockaddr_in *dest_addr, int seq);
int recv_response(int socket_fd, int seq, double *rtt, struct sockaddr_in *from_addr, int *icmp_type, int *is_reply_from_target_flag);

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "使用方法: %s <目标IP地址>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *target_ip_str = argv[1];
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, target_ip_str, &dest_addr.sin_addr) <= 0) {
        fprintf(stderr, "无效的IP地址: %s\n", target_ip_str);
        return EXIT_FAILURE;
    }

    int sockfd;
    if ((sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)) < 0) {
        perror("创建原始套接字失败");
        fprintf(stderr, "请确保您以root权限运行此程序 (例如: sudo %s %s)\n", argv[0], target_ip_str);
        return EXIT_FAILURE;
    }

    struct timeval recv_timeout;
    recv_timeout.tv_sec = 2; // 2秒接收超时
    recv_timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout)) < 0) {
        perror("设置套接字接收超时失败");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("tracert 到 %s (%s), 最大跳数 %d\n", target_ip_str, inet_ntoa(dest_addr.sin_addr), 30);

    int current_seq = 0;
    int max_ttl_val = 30;
    int probes_per_hop = 3;
    int target_reached = 0;

    for (int ttl = 1; ttl <= max_ttl_val && !target_reached; ++ttl) {
        printf("%2d ", ttl);
        fflush(stdout);

        if (setsockopt(sockfd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
            perror("设置IP_TTL失败");
            fprintf(stderr, "警告: TTL %d 设置失败, 跳过此TTL的探测\n", ttl);
            printf("  * * *\n");
            continue;
        }

        char current_hop_printed_ip[INET_ADDRSTRLEN] = ""; // 存储当前跳已打印的IP
        int ip_has_been_printed_for_this_ttl = 0;

        for (int probe_num = 0; probe_num < probes_per_hop; ++probe_num) {
            struct sockaddr_in hop_addr; // 记录每一跳的地址
            double rtt_ms = 0;
            int received_icmp_type = 0;
            int is_final_target_reply = 0;

            send_probe(sockfd, &dest_addr, current_seq);

            int recv_status = recv_response(sockfd, current_seq, &rtt_ms, &hop_addr, &received_icmp_type, &is_final_target_reply);
            current_seq++;

            if (recv_status == 0 || recv_status == 2) { // 0: IP和RTT成功, 2: IP成功, RTT失败
                char *current_reply_ip = inet_ntoa(hop_addr.sin_addr);
                if (!ip_has_been_printed_for_this_ttl) {
                    strncpy(current_hop_printed_ip, current_reply_ip, INET_ADDRSTRLEN -1);
                    current_hop_printed_ip[INET_ADDRSTRLEN-1] = '\0';
                    printf(" %s", current_hop_printed_ip);
                    ip_has_been_printed_for_this_ttl = 1;
                } else if (strcmp(current_hop_printed_ip, current_reply_ip) != 0) {
                    // IP发生变化，用括号补充显示
                    printf(" (%s)", current_reply_ip);
                }
                // else IP未变且已打印，则不重复打印IP

                if (recv_status == 0) { // 成功获取RTT
                    printf("  %.3f ms", rtt_ms);
                } else { // recv_status == 2, RTT获取失败
                    printf("  *");
                }
            } else { // recv_status == 1, 完全超时或错误
                if (!ip_has_been_printed_for_this_ttl && probe_num == 0) {
                    // 如果是此TTL的第一次探测就失败，为了对齐，可能需要一个占位空格
                    // 但如果前面已经有IP了，就不需要这个空格了。
                    // 简单起见，这里如果前面没有IP，就直接打星号，对齐交给多个星号间的空格
                }
                printf("  *");
            }
            fflush(stdout);

            if (is_final_target_reply) {
                target_reached = 1;
            }
        }
        printf("\n");

        if (target_reached) {
            printf("已到达目标 %s\n", target_ip_str);
            break; // 跳出外层TTL循环
        }
    }

    close(sockfd);
    return EXIT_SUCCESS;
}

uint16_t compute_checksum(uint16_t *addr, int len) {
    long sum = 0; // 使用long防止累加时溢出
    uint16_t *current_word = addr;
    int bytes_left = len;
    uint16_t odd_byte_word = 0;

    while (bytes_left > 1) {
        sum += *current_word++;
        bytes_left -= 2;
    }

    if (bytes_left == 1) {
        *(unsigned char *)(&odd_byte_word) = *(unsigned char *)current_word;
        sum += odd_byte_word;
    }

    // 将32位和折叠成16位
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

void send_probe(int socket_fd, const struct sockaddr_in *dest_addr, int seq) {
    char packet_buffer[64]; // ICMP头8字节 + 时间戳(约16字节) = 24字节，64字节足够
    struct icmp *icmp_packet = (struct icmp *)packet_buffer;
    memset(packet_buffer, 0, sizeof(packet_buffer));

    icmp_packet->icmp_type = ICMP_ECHO;
    icmp_packet->icmp_code = 0;
    icmp_packet->icmp_id = htons(getpid() & 0xFFFF); // 使用进程ID作为ICMP ID
    icmp_packet->icmp_seq = htons(seq);

    struct timeval send_timestamp;
    gettimeofday(&send_timestamp, NULL);
    // 将时间戳复制到ICMP数据部分 (ICMP头部之后)
    memcpy((char *)icmp_packet + 8, &send_timestamp, sizeof(send_timestamp));

    int packet_total_size = 8 + sizeof(send_timestamp); // ICMP头 + 时间戳数据
    icmp_packet->icmp_cksum = 0; // 计算校验和前必须将校验和字段置0
    icmp_packet->icmp_cksum = compute_checksum((uint16_t *)icmp_packet, packet_total_size);

    DEBUG_PRINT("发送探测包 (ID: %d, Seq: %d) 到 %s, TTL(外部设置), 大小: %d\n",
                ntohs(icmp_packet->icmp_id), ntohs(icmp_packet->icmp_seq), inet_ntoa(dest_addr->sin_addr), packet_total_size);

    if (sendto(socket_fd, packet_buffer, packet_total_size, 0, (struct sockaddr *)dest_addr, sizeof(*dest_addr)) <= 0) {
        DEBUG_PRINT("发送探测包失败 (Seq: %d): %s\n", seq, strerror(errno));
    }
}

// 返回值: 0 = 成功获取IP和RTT, 1 = 超时/错误/无效包, 2 = 成功获取IP但RTT失败
int recv_response(int socket_fd, int seq_to_match, double *rtt, struct sockaddr_in *from_addr, int *icmp_type_received, int *is_reply_from_target_flag) {
    char recv_buffer[1024];
    socklen_t from_addr_len = sizeof(*from_addr);
    struct ip *ip_header;
    struct icmp *icmp_header;
    struct timeval time_of_recv;

    memset(from_addr, 0, sizeof(*from_addr)); // 初始化返回地址结构

    int bytes_received = recvfrom(socket_fd, recv_buffer, sizeof(recv_buffer), 0, (struct sockaddr *)from_addr, &from_addr_len);

    if (bytes_received <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) { // EAGAIN和EWOULDBLOCK通常表示超时
            DEBUG_PRINT("接收超时 (期望Seq: %d)\n", seq_to_match);
        } else {
            DEBUG_PRINT("接收数据包失败 (期望Seq: %d): %s\n", seq_to_match, strerror(errno));
        }
        return 1; // 超时或接收错误
    }

    gettimeofday(&time_of_recv, NULL); // 记录接收到数据包的时间
    DEBUG_PRINT("接收到来自 %s 的数据包，长度: %d 字节\n", inet_ntoa(from_addr->sin_addr), bytes_received);

    ip_header = (struct ip *)recv_buffer;
    int ip_header_length = ip_header->ip_hl * 4; // IP头部长度（以字节为单位）

    if (ip_header_length < 20) { // IP头部至少20字节
        DEBUG_PRINT("无效的IP头部长度: %d字节\n", ip_header_length);
        return 1; // 无效包
    }
    if (ip_header_length + 8 > bytes_received) { // ICMP头部至少8字节
        DEBUG_PRINT("数据包太短，无法获取ICMP头部 (IP头长 %d, 总长 %d)\n", ip_header_length, bytes_received);
        return 1; // 无效包
    }

    icmp_header = (struct icmp *)(recv_buffer + ip_header_length);
    *icmp_type_received = icmp_header->icmp_type;
    uint16_t received_icmp_id = ntohs(icmp_header->icmp_id);
    // uint16_t received_icmp_seq = ntohs(icmp_header->icmp_seq); // 对于TIME_EXCEEDED，这是原始包的seq

    DEBUG_PRINT("解析ICMP: 类型=%d, Code=%d, ID=%u, Seq=%u (来自 %s)\n",
                *icmp_type_received, icmp_header->icmp_code, received_icmp_id,
                ntohs(icmp_header->icmp_seq), inet_ntoa(from_addr->sin_addr));

    int rtt_successfully_calculated = 0;
    uint16_t expected_process_id = getpid() & 0xFFFF;

    if (*icmp_type_received == ICMP_TIME_EXCEEDED) {
        DEBUG_PRINT("收到TTL超时消息 from %s\n", inet_ntoa(from_addr->sin_addr));
        // ICMP_TIME_EXCEEDED 负载: 原始IP头 + 原始数据的前8字节(即我们的ICMP Echo请求头)
        struct ip *original_ip_header = (struct ip *)((char *)icmp_header + 8); // 跳过8字节的ICMP_TIME_EXCEEDED头
        int original_ip_header_length = original_ip_header->ip_hl * 4;

        // 确保有足够的空间来读取原始IP头和原始ICMP头
        if (ip_header_length + 8 + original_ip_header_length + 8 > bytes_received) {
            DEBUG_PRINT("数据包太短 (TIME_EXCEEDED)，无法获取完整的原始ICMP头部\n");
            return 2; // 获取到IP，但RTT失败
        }
        struct icmp *original_icmp_header = (struct icmp *)((char *)original_ip_header + original_ip_header_length);
        uint16_t original_icmp_id = ntohs(original_icmp_header->icmp_id);
        // uint16_t original_icmp_seq = ntohs(original_icmp_header->icmp_seq);

        DEBUG_PRINT("原始ICMP (TIME_EXCEEDED负载中): ID=%u (期望PID=%u), Seq=%u, Type=%d\n",
                   original_icmp_id, expected_process_id, ntohs(original_icmp_header->icmp_seq), original_icmp_header->icmp_type);

        if (original_icmp_header->icmp_type == ICMP_ECHO && original_icmp_id == expected_process_id) {
            // 尝试从原始ICMP数据中提取时间戳
            if (ip_header_length + 8 + original_ip_header_length + 8 + sizeof(struct timeval) <= bytes_received) {
                struct timeval original_send_timestamp;
                memcpy(&original_send_timestamp, (char *)original_icmp_header + 8, sizeof(struct timeval));
                *rtt = (time_of_recv.tv_sec - original_send_timestamp.tv_sec) * 1000.0 +
                       (time_of_recv.tv_usec - original_send_timestamp.tv_usec) / 1000.0;
                rtt_successfully_calculated = 1;
                DEBUG_PRINT("成功从TTL超时消息中解析RTT: %.3f ms\n", *rtt);
            } else {
                DEBUG_PRINT("数据包太短 (TIME_EXCEEDED)，无法获取原始时间戳数据部分\n");
            }
        } else {
             DEBUG_PRINT("TIME_EXCEEDED 中封装的原始ICMP ID或类型不匹配\n");
             // 即使ID不匹配，仍然收到了一个TIME_EXCEEDED，所以IP是有效的，但不能算作对我们探测的精确响应
             // 严格来说，这可能是一个错误，但为了traceroute能显示中间IP，我们仍认为IP获取成功
        }
    } else if (*icmp_type_received == ICMP_ECHOREPLY) {
        DEBUG_PRINT("收到Echo回复 from %s\n", inet_ntoa(from_addr->sin_addr));
        if (received_icmp_id == expected_process_id) {
            // 确认这是对我们发出的探测的直接回复
            *is_reply_from_target_flag = 1;
            // 尝试从ICMP Echo回复的数据部分提取时间戳
            if (ip_header_length + 8 + sizeof(struct timeval) <= bytes_received) {
                struct timeval original_send_timestamp;
                memcpy(&original_send_timestamp, (char *)icmp_header + 8, sizeof(struct timeval));
                *rtt = (time_of_recv.tv_sec - original_send_timestamp.tv_sec) * 1000.0 +
                       (time_of_recv.tv_usec - original_send_timestamp.tv_usec) / 1000.0;
                rtt_successfully_calculated = 1;
                DEBUG_PRINT("成功从Echo回复中解析RTT: %.3f ms\n", *rtt);
            } else {
                DEBUG_PRINT("数据包太短 (ECHOREPLY)，无法获取时间戳数据部分\n");
            }
        } else {
            DEBUG_PRINT("Echo回复的ID (%u) 与期望ID (%u) 不匹配\n", received_icmp_id, expected_process_id);
            return 1; // ID不匹配，不是我们期望的回复
        }
    } else {
        DEBUG_PRINT("收到未处理的ICMP类型: %d from %s\n", *icmp_type_received, inet_ntoa(from_addr->sin_addr));
        return 1; // 其他ICMP类型，视为无效响应
    }

    if (rtt_successfully_calculated) {
        return 0; // 成功获取IP和RTT
    } else {
        // 即使RTT计算失败，如果收到了有效的TIME_EXCEEDED或已确认的ECHOREPLY（ID匹配）
        // 我们也认为IP地址是有效的。
        if (*icmp_type_received == ICMP_TIME_EXCEEDED || *is_reply_from_target_flag) {
            return 2; // 成功获取IP，但RTT失败
        }
        return 1; // 其他情况视为获取IP失败
    }
}