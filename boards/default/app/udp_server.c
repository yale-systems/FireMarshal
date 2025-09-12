#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdint.h>

#define BUF_SIZE 1500   // Typical MTU; adjust if you expect larger packets

int main() {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    unsigned char buffer[BUF_SIZE];

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Prepare server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(1111);
    if (inet_aton("10.0.0.2", &server_addr.sin_addr) == 0) {
        fprintf(stderr, "Invalid IP address\n");
        exit(EXIT_FAILURE);
    }

    // Bind the socket
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("UDP server listening on 10.0.0.2:1111\n");

    while (1) {
        // Receive a datagram
        ssize_t n = recvfrom(sockfd, buffer, BUF_SIZE, 0,
                             (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) {
            perror("recvfrom failed");
            continue;
        }

        // printf("Received %zd bytes\n", n);
        if (n > 0) {
            // printf("First byte before: %u\n", buffer[0]);
            buffer[0] = (buffer[0] + 1) & 0xFF;  // increment first byte
            // printf("First byte after: %u\n", buffer[0]);
        }

        // Send back same size data
        if (sendto(sockfd, buffer, n, 0,
                   (struct sockaddr *)&client_addr, addr_len) < 0) {
            perror("sendto failed");
        } else {
            // printf("Sent back %zd bytes\n", n);
        }
    }

    close(sockfd);
    return 0;
}
