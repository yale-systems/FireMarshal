#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUF_SIZE 4096

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    char buffer[BUF_SIZE];
    FILE *fp;

    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(1);
    }

    // Bind
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(1);
    }

    // Listen
    if (listen(server_fd, 3) < 0) {
        perror("listen failed");
        exit(1);
    }
    printf("Server listening on port %d...\n", PORT);

    // Accept
    if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
                             (socklen_t *)&addrlen)) < 0) {
        perror("accept failed");
        exit(1);
    }

    // --- Receive filename first ---
    uint32_t fname_len;
    if (recv(new_socket, &fname_len, sizeof(fname_len), 0) <= 0) {
        perror("recv filename length failed");
        close(new_socket);
        return 1;
    }
    fname_len = ntohl(fname_len); // convert from network byte order

    char filename[256];
    if (fname_len >= sizeof(filename)) {
        fprintf(stderr, "Filename too long!\n");
        close(new_socket);
        return 1;
    }

    if (recv(new_socket, filename, fname_len, 0) <= 0) {
        perror("recv filename failed");
        close(new_socket);
        return 1;
    }
    filename[fname_len] = '\0'; // null terminate

    printf("Receiving file: %s\n", filename);

    // Open output file with same name
    fp = fopen(filename, "wb");
    if (fp == NULL) {
        perror("File open failed");
        close(new_socket);
        return 1;
    }

    // --- Receive file content ---
    ssize_t n;
    while ((n = recv(new_socket, buffer, BUF_SIZE, 0)) > 0) {
        fwrite(buffer, 1, n, fp);
    }

    printf("File '%s' received successfully.\n", filename);
    fclose(fp);
    close(new_socket);
    close(server_fd);
    return 0;
}
