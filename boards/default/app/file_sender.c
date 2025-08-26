#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <libgen.h>   // for basename()

#define PORT 8080
#define BUF_SIZE 4096

int main(int argc, char *argv[]) {
    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[BUF_SIZE];
    FILE *fp;

    if (argc != 3) {
        printf("Usage: %s <server_ip> <filename>\n", argv[0]);
        return -1;
    }

    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket creation failed");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) <= 0) {
        perror("Invalid address");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection failed");
        return -1;
    }

    // Extract just the filename (strip path)
    char *fname = basename(argv[2]);
    uint32_t fname_len = strlen(fname);

    // Send filename length
    uint32_t net_len = htonl(fname_len);
    if (send(sock, &net_len, sizeof(net_len), 0) < 0) {
        perror("send filename length failed");
        return -1;
    }

    // Send filename
    if (send(sock, fname, fname_len, 0) < 0) {
        perror("send filename failed");
        return -1;
    }

    // Open file
    fp = fopen(argv[2], "rb");
    if (fp == NULL) {
        perror("File open failed");
        return -1;
    }

    // Send file contents
    size_t n;
    while ((n = fread(buffer, 1, BUF_SIZE, fp)) > 0) {
        if (send(sock, buffer, n, 0) < 0) {
            perror("send file failed");
            fclose(fp);
            return -1;
        }
    }

    printf("File '%s' sent successfully.\n", fname);
    fclose(fp);
    close(sock);
    return 0;
}
