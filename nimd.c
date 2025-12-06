#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/time.h>
#include <ctype.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_NAME 72
#define BUF_SIZE 256
#define MSG_BODY_SIZE 100   // message body length is at most 99

// global piles for the Nim game
int piles[5] = {1, 3, 5, 7, 9};

typedef struct {
    int fd;
    char name[MAX_NAME + 1];
} Player;

// function headers
void play_game(Player p1, Player p2);
int handle_message(Player *me, Player *opp, int *turn, int my_id);
void send_fail(int fd, char *code, char *msg, int close_conn);
void send_over(int fd1, int fd2, int winner, char *reason);
void broadcast_play(int fd1, int fd2, int next_player);
void sigchld_handler(int s);
void disable_nagle(int fd);

// main server program
int main(int argc, char *argv[]) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);
    int port;

    char buf[BUF_SIZE];
    char *ptr;
    int pipes_count;
    char *name_start;
    char *name_end;
    int valread;

    Player waiting_player;
    int waiting = 0;     // 0 means nobody waiting, 1 means one player waiting
    int pid;

    fd_set readfds;
    int max_sd;
    int activity;

    const char wait_msg[] = "0|05|WAIT|";

    setvbuf(stdout, NULL, _IONBF, 0);

    signal(SIGCHLD, sigchld_handler);
    signal(SIGPIPE, SIG_IGN);

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    port = atoi(argv[1]);

    // create listening socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket failed");
        exit(1);
    }

    // allow reuse of address
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    // set up server address
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // bind socket to port
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(1);
    }

    // start listening
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(1);
    }

    printf("Server listening on port %d...\n", port);

    // main loop: accept new players and handle waiting player state
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        max_sd = server_fd;

        // if someone is waiting, watch their socket too
        if (waiting) {
            FD_SET(waiting_player.fd, &readfds);
            if (waiting_player.fd > max_sd) {
                max_sd = waiting_player.fd;
            }
        }

        activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) {
            if (errno == EINTR) continue;
            perror("select");
            continue;
        }

        // new incoming connection on listening socket
        if (FD_ISSET(server_fd, &readfds)) {
            new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
            if (new_socket < 0) {
                if (errno == EINTR) continue;
                perror("accept");
                continue;
            }

            disable_nagle(new_socket);

            memset(buf, 0, BUF_SIZE);
            valread = read(new_socket, buf, BUF_SIZE);
            if (valread <= 0) {
                close(new_socket);
                continue;
            }

            // first byte must be 0 and message must contain OPEN
            if (buf[0] != '0') {
                close(new_socket);
                continue;
            }

            if (strstr(buf, "OPEN") == NULL) {
                send_fail(new_socket, (char *)"10", (char *)"Invalid", 1);
                continue;
            }

            // find player name in OPEN message
            ptr = buf;
            pipes_count = 0;
            name_start = NULL;

            while (*ptr) {
                if (*ptr == '|') {
                    pipes_count++;
                    if (pipes_count == 3) {
                        name_start = ptr + 1;
                        break;
                    }
                }
                ptr++;
            }

            if (name_start == NULL) {
                send_fail(new_socket, (char *)"10", (char *)"Invalid", 1);
                continue;
            }

            name_end = strchr(name_start, '|');
            if (name_end != NULL) {
                *name_end = '\0';
            }

            if (strlen(name_start) > MAX_NAME) {
                send_fail(new_socket, (char *)"21", (char *)"Long Name", 1);
                continue;
            }

            Player temp;
            temp.fd = new_socket;
            strcpy(temp.name, name_start);

            // tell new player to wait
            write(new_socket, wait_msg, strlen(wait_msg));

            // if no one is waiting, store this player
            if (!waiting) {
                waiting_player = temp;
                waiting = 1;
                printf("Player %s is waiting.\n", waiting_player.name);
            } else {
                // if someone is waiting, start a new game with two players
                printf("Matching %s with %s.\n",
                       waiting_player.name, temp.name);

                pid = fork();
                if (pid == 0) {
                    // child process handles the actual game
                    close(server_fd);
                    play_game(waiting_player, temp);
                    exit(0);
                } else if (pid > 0) {
                    // parent closes player sockets and goes back to waiting
                    close(waiting_player.fd);
                    close(temp.fd);
                    waiting = 0;
                } else {
                    perror("fork");
                    close(new_socket);
                }
            }
        }

        // handle extra messages from the waiting player
        if (waiting && FD_ISSET(waiting_player.fd, &readfds)) {
            memset(buf, 0, BUF_SIZE);
            valread = read(waiting_player.fd, buf, BUF_SIZE);

            // if waiting player disconnects
            if (valread <= 0) {
                printf("Waiting player %s disconnected before match.\n",
                       waiting_player.name);
                close(waiting_player.fd);
                waiting = 0;
                continue;
            }

            // waiting player sent some message, check it
            if (buf[0] != '0') {
                send_fail(waiting_player.fd, (char *)"10", (char *)"Invalid", 1);
                waiting = 0;
                continue;
            }

            char type[5];
            memset(type, 0, sizeof(type));
            strncpy(type, buf + 5, 4);
            type[4] = '\0';

            // second OPEN on same connection
            if (strcmp(type, "OPEN") == 0) {
                send_fail(waiting_player.fd, (char *)"23", (char *)"Already Open", 1);
            }
            // MOVE while not in a game yet
            else if (strcmp(type, "MOVE") == 0) {
                send_fail(waiting_player.fd, (char *)"24", (char *)"Not Playing", 1);
            }
            // any other bad message
            else {
                send_fail(waiting_player.fd, (char *)"10", (char *)"Invalid", 1);
            }

            waiting = 0;
        }
    }

    return 0;
}

// handle SIGCHLD to clean up child processes
void sigchld_handler(int s) {
    (void)s;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void disable_nagle(int fd) {
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag));
}

// handle a single message from one player during a game
int handle_message(Player *me, Player *opp, int *turn, int my_id) {
    char buf[BUF_SIZE];
    int n;
    char type[5];
    char *pile_str, *count_str, *split, *end;
    int pile_idx, count;

    memset(buf, 0, BUF_SIZE);

    // read one message, retry if interrupted
    do {
        n = read(me->fd, buf, BUF_SIZE);
    } while (n < 0 && errno == EINTR);

    // if read <= 0, player disconnected (forfeit)
    if (n <= 0) {
        printf("Player %s disconnected (forfeit).\n", me->name);
        return -1;
    }

    // check NGP version
    if (buf[0] != '0') {
        send_fail(me->fd, (char *)"10", (char *)"Invalid", 1);
        return -1;
    }

    memset(type, 0, sizeof(type));
    strncpy(type, buf + 5, 4);
    type[4] = '\0';

    // second OPEN during game is not allowed
    if (strcmp(type, "OPEN") == 0) {
        send_fail(me->fd, (char *)"23", (char *)"Already Open", 1);
        return -1;
    }

    // only MOVE messages are valid here
    if (strcmp(type, "MOVE") != 0) {
        send_fail(me->fd, (char *)"10", (char *)"Invalid", 1);
        return -1;
    }

    // check if it is this player's turn
    if (*turn != my_id) {
        send_fail(me->fd, (char *)"31", (char *)"Impatient", 0);
        return 2;
    }

    // parse pile index and count from message
    pile_str = buf + 10;

    split = strchr(pile_str, '|');
    if (!split) {
        send_fail(me->fd, (char *)"10", (char *)"Invalid", 1);
        return -1;
    }

    *split = '\0';
    count_str = split + 1;

    end = strchr(count_str, '|');
    if (end) *end = '\0';

    pile_idx = atoi(pile_str);
    count    = atoi(count_str);

    // check pile index range
    if (pile_idx < 0 || pile_idx > 4) {
        send_fail(me->fd, (char *)"32", (char *)"Pile Index", 0);
        return 2;
    }

    // check count range
    if (count < 1 || count > piles[pile_idx]) {
        send_fail(me->fd, (char *)"33", (char *)"Quantity", 0);
        return 2;
    }

    // apply move to the board
    piles[pile_idx] -= count;
    printf("Player %s removed %d from pile %d\n",
           me->name, count, pile_idx);

    return 1;
}

// run one Nim game between two players
void play_game(Player p1, Player p2) {
    char buf[BUF_SIZE];
    char body[MSG_BODY_SIZE];
    int current_turn = 1;
    int game_running = 1;
    int max_sd, activity, result, i, stones_left;
    fd_set readfds;

    // set initial piles to 1, 3, 5, 7, 9
    for (i = 0; i < 5; i++) {
        piles[i] = 2 * i + 1;
    }

    // tell player 1 about player 2
    snprintf(body, sizeof(body), "NAME|1|%s|", p2.name);
    int len = (int)strlen(body);
    if (len > 99) len = 99;
    snprintf(buf, sizeof(buf), "0|%02d|%s", len, body);
    write(p1.fd, buf, strlen(buf));

    // tell player 2 about player 1
    snprintf(body, sizeof(body), "NAME|2|%s|", p1.name);
    len = (int)strlen(body);
    if (len > 99) len = 99;
    snprintf(buf, sizeof(buf), "0|%02d|%s", len, body);
    write(p2.fd, buf, strlen(buf));

    // send initial board state
    broadcast_play(p1.fd, p2.fd, current_turn);

    // main game loop
    while (game_running) {
        FD_ZERO(&readfds);
        FD_SET(p1.fd, &readfds);
        FD_SET(p2.fd, &readfds);

        max_sd = (p1.fd > p2.fd ? p1.fd : p2.fd);

        activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) {
            if (errno == EINTR) continue;
            perror("select error");
            break;
        }

        // handle input from player 1
        if (FD_ISSET(p1.fd, &readfds)) {
            result = handle_message(&p1, &p2, &current_turn, 1);

            // result < 0 means p1 forfeited or bad error
            if (result < 0) {
                send_over(p2.fd, -1, 2, (char *)"Forfeit");
                game_running = 0;
                break;
            }

            // result == 1 means valid move
            if (result == 1) {
                stones_left = 0;
                for (i = 0; i < 5; i++) stones_left += piles[i];

                // if no stones left, player 1 wins normally
                if (stones_left == 0) {
                    send_over(p1.fd, p2.fd, 1, (char *)"");
                    game_running = 0;
                    break;
                }

                // switch turn to player 2
                current_turn = 2;
                broadcast_play(p1.fd, p2.fd, current_turn);
            }
        }

        // handle input from player 2
        if (game_running && FD_ISSET(p2.fd, &readfds)) {
            result = handle_message(&p2, &p1, &current_turn, 2);

            // result < 0 means p2 forfeited or bad error
            if (result < 0) {
                send_over(p1.fd, -1, 1, (char *)"Forfeit");
                game_running = 0;
                break;
            }

            // result == 1 means valid move
            if (result == 1) {
                stones_left = 0;
                for (i = 0; i < 5; i++) stones_left += piles[i];

                // if no stones left, player 2 wins normally
                if (stones_left == 0) {
                    send_over(p1.fd, p2.fd, 2, (char *)"");
                    game_running = 0;
                    break;
                }

                // switch turn to player 1
                current_turn = 1;
                broadcast_play(p1.fd, p2.fd, current_turn);
            }
        }
    }

    // close both sockets at end of game
    shutdown(p1.fd, SHUT_WR);
    shutdown(p2.fd, SHUT_WR);
    sleep(1);
    close(p1.fd);
    close(p2.fd);
}

// send PLAY message to both players
void broadcast_play(int fd1, int fd2, int next_player) {
    char buf[BUF_SIZE];
    char body[MSG_BODY_SIZE];
    char piles_str[50];

    snprintf(piles_str, sizeof(piles_str),
             "%d %d %d %d %d",
             piles[0], piles[1], piles[2], piles[3], piles[4]);

    snprintf(body, sizeof(body), "PLAY|%d|%s|", next_player, piles_str);
    int len = (int)strlen(body);
    if (len > 99) len = 99;
    snprintf(buf, sizeof(buf), "0|%02d|%s", len, body);

    write(fd1, buf, strlen(buf));
    write(fd2, buf, strlen(buf));

    printf("Sent PLAY. Next: %d. Board: %s\n", next_player, piles_str);
}

// send OVER message to one or two players
void send_over(int fd1, int fd2, int winner, char *reason) {
    char buf[BUF_SIZE];
    char body[MSG_BODY_SIZE];
    char piles_str[50];

    snprintf(piles_str, sizeof(piles_str),
             "%d %d %d %d %d",
             piles[0], piles[1], piles[2], piles[3], piles[4]);

    snprintf(body, sizeof(body), "OVER|%d|%s|%s|",
             winner, piles_str, reason);
    int len = (int)strlen(body);
    if (len > 99) len = 99;
    snprintf(buf, sizeof(buf), "0|%02d|%s", len, body);

    if (fd1 != -1) write(fd1, buf, strlen(buf));
    if (fd2 != -1) write(fd2, buf, strlen(buf));

    printf("Game Over. Winner: %d. Reason: %s\n", winner, reason);
}

// send FAIL message and maybe close connection
void send_fail(int fd, char *code, char *msg, int close_conn) {
    char buf[BUF_SIZE];
    char body[MSG_BODY_SIZE];

    snprintf(body, sizeof(body), "FAIL|%s|%s|", code, msg);
    int len = (int)strlen(body);
    if (len > 99) len = 99;
    snprintf(buf, sizeof(buf), "0|%02d|%s", len, body);

    write(fd, buf, strlen(buf));

    if (close_conn) {
        shutdown(fd, SHUT_WR);
        close(fd);
    }
}
