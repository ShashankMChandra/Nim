#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>

#define BUF_SIZE 1024

// Connect to server
int connect_to_server(const char *hostname, const char *port) {
    int sockfd;
    struct addrinfo hints, *servinfo, *info;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if(getaddrinfo(hostname, port, &hints, &servinfo) != 0) return -1;

    for(info = servinfo; info != NULL; info = info->ai_next) {
        if((sockfd = socket(info->ai_family, info->ai_socktype, info->ai_protocol)) == -1) continue;
        if(connect(sockfd, info->ai_addr, info->ai_addrlen) == -1) {
            close(sockfd);
            continue;
        }
        break;
    }

    freeaddrinfo(servinfo);
    return (info == NULL) ? -1 : sockfd;
}

void send_raw(int fd, const char *msg) {
    write(fd, msg, strlen(msg));
}

// Send formatted NGP message
void send_ngp(int fd, const char *body) {
    char msg[BUF_SIZE];
    snprintf(msg, sizeof(msg), "0|%02lu|%s", strlen(body), body);
    write(fd, msg, strlen(msg));
}

// Read until expected string found or timeout
int expect_response(int fd, const char *expected) {
    char buf[4096] = {0}; 
    int len = 0;

    // Try 10 times (1s total)
    for(int i = 0; i < 10; i++) {
        int n = recv(fd, buf + len, sizeof(buf) - len - 1, MSG_DONTWAIT);
        if(n > 0) len += n; 
        if(strstr(buf, expected)) return 1; 
        usleep(100000); 
    }
    return 0;
}

void run_test_10(const char *host, const char *port) {
    int fd = connect_to_server(host, port);
    if(fd < 0) { printf("Test 1 (Error 10): FAIL\n"); return; }
    
    // Bad Start Char
    send_raw(fd, "X|99|GARBAGE|");
    close(fd); 

    // Bad Format
    fd = connect_to_server(host, port);
    send_raw(fd, "0|00|BADFORMAT|");
    
    if(expect_response(fd, "FAIL|10")) printf("Test 1 (Error 10): PASS\n");
    else printf("Test 1 (Error 10): FAIL\n");
    close(fd);
}

void run_test_21(const char *host, const char *port) {
    int fd = connect_to_server(host, port);

    char long_name[80];
    memset(long_name, 'A', 73);
    long_name[73] = '\0';
    
    char body[100];
    snprintf(body, sizeof(body), "OPEN|%s|", long_name);
    
    send_ngp(fd, body);
    if(expect_response(fd, "FAIL|21")) printf("Test 2 (Error 21): PASS\n");
    else printf("Test 2 (Error 21): FAIL\n");
    close(fd);
}

void run_test_23(const char *host, const char *port) {
    int fd = connect_to_server(host, port);

    send_ngp(fd, "OPEN|DoubleOpen|");
    expect_response(fd, "WAIT"); 
    
    send_ngp(fd, "OPEN|DoubleOpen|");
    if(expect_response(fd, "FAIL|23")) printf("Test 3 (Error 23): PASS\n");
    else printf("Test 3 (Error 23): FAIL\n");
    close(fd);
}

void run_test_24(const char *host, const char *port) {
    int fd = connect_to_server(host, port);

    send_ngp(fd, "OPEN|Early|");
    expect_response(fd, "WAIT"); 
    
    send_ngp(fd, "MOVE|1|1|");
    if(expect_response(fd, "FAIL|24")) printf("Test 4 (Error 24): PASS\n");
    else printf("Test 4 (Error 24): FAIL\n");
    close(fd);
}

void run_test_22(const char *host, const char *port) {
    int fd1 = connect_to_server(host, port);
    send_ngp(fd1, "OPEN|OccupiedName|");
    expect_response(fd1, "WAIT");

    int fd2 = connect_to_server(host, port);
    send_ngp(fd2, "OPEN|OccupiedName|");
    
    if(expect_response(fd2, "FAIL|22")) printf("Test 5 (Error 22): PASS\n");
    else printf("Test 5 (Error 22): FAIL\n");
    
    close(fd1);
    close(fd2);
}

void game_errors(const char *host, const char *port) {
    int p1 = connect_to_server(host, port);
    int p2 = connect_to_server(host, port);

    if(p1 < 0 || p2 < 0) {
        printf("Test 6 (Error 31): FAIL\nTest 7 (Error 32): FAIL\nTest 8 (Error 33): FAIL\n");
        return;
    }

    send_ngp(p1, "OPEN|Player1|");
    expect_response(p1, "WAIT"); 
    send_ngp(p2, "OPEN|Player2|");
    
    // Ensure game started
    if(!expect_response(p1, "PLAY") || !expect_response(p2, "PLAY")) {
        printf("Test 6 (Error 31): FAIL\nTest 7 (Error 32): FAIL\nTest 8 (Error 33): FAIL\n");
        close(p1); close(p2);
        return;
    }

    // Test 6: Move out of turn (Error 31)
    send_ngp(p2, "MOVE|0|1|");
    if(expect_response(p2, "FAIL|31")) printf("Test 6 (Error 31): PASS\n");
    else printf("Test 6 (Error 31): FAIL\n");

    // Test 7: Invalid pile (Error 32)
    send_ngp(p1, "MOVE|5|1|");
    if(expect_response(p1, "FAIL|32")) printf("Test 7 (Error 32): PASS\n");
    else printf("Test 7 (Error 32): FAIL\n");

    // Test 8: Invalid quantity (Error 33)
    send_ngp(p1, "MOVE|0|2|");
    if(expect_response(p1, "FAIL|33")) printf("Test 8 (Error 33): PASS\n");
    else printf("Test 8 (Error 33): FAIL\n");

    close(p1);
    close(p2);
}

int main(int argc, char *argv[]) {
    if(argc != 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        exit(1);
    }
    run_test_10(argv[1], argv[2]);
    run_test_21(argv[1], argv[2]);
    run_test_23(argv[1], argv[2]);
    run_test_24(argv[1], argv[2]);
    run_test_22(argv[1], argv[2]);
    game_errors(argv[1], argv[2]);
    return 0;
}