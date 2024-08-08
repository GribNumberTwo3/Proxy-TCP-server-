#include <iostream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <map>
#include <vector>
#include <queue>
#include <fstream>
#include <string>
#include <chrono>
#include <iomanip>
#include <ctime>

#define PROXY_PORT 5433 // Порт для прокси-сервера
#define PG_PORT 5432    // Порт PostgreSQL
#define BUFFER_SIZE 4096
#define MAX_EVENTS 20   // Увеличиваем количество обрабатываемых событий

std::string getCurrentDateTime() {
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&now_c);
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void appendToFile(const std::string& fileName, const std::string& text) {
    std::ofstream file(fileName, std::ios::app);
    if (file.is_open()) {
        file << getCurrentDateTime() << " - " << text << std::endl;
        file.close();
    } else {
        std::cerr << "Не удалось открыть файл для записи." << std::endl;
    }
}

struct Client {
    int fd;
    int pg_fd;
    std::queue<std::vector<char>> outbox; // Очередь данных для отправки
};

int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int createAndBindSocket(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, SOMAXCONN) == -1) {
        perror("listen");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    return listen_fd;
}

void handleConnection(int epoll_fd, int listen_fd, std::map<int, Client>& clients) {
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd == -1) {
        perror("accept");
        return;
    }

    if (setNonBlocking(client_fd) == -1) {
        perror("setNonBlocking");
        close(client_fd);
        return;
    }

    int pg_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (pg_fd == -1) {
        perror("socket");
        close(client_fd);
        return;
    }

    sockaddr_in pg_addr;
    memset(&pg_addr, 0, sizeof(pg_addr));
    pg_addr.sin_family = AF_INET;
    pg_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    pg_addr.sin_port = htons(PG_PORT);

    if (connect(pg_fd, (struct sockaddr*)&pg_addr, sizeof(pg_addr)) == -1) {
        perror("connect to PostgreSQL");
        close(client_fd);
        close(pg_fd);
        return;
    }

    if (setNonBlocking(pg_fd) == -1) {
        perror("setNonBlocking");
        close(client_fd);
        close(pg_fd);
        return;
    }

    epoll_event event;
    event.data.fd = client_fd;
    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
        perror("epoll_ctl: client_fd");
        close(client_fd);
        close(pg_fd);
        return;
    }

    event.data.fd = pg_fd;
    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pg_fd, &event) == -1) {
        perror("epoll_ctl: pg_fd");
        close(client_fd);
        close(pg_fd);
        return;
    }

    clients[client_fd] = {client_fd, pg_fd, {}};
    clients[pg_fd] = {client_fd, pg_fd, {}};
}

void handleClientData(int epoll_fd, int client_fd, std::map<int, Client>& clients) {
    char buffer[BUFFER_SIZE];
    while (true) {
        ssize_t count = read(client_fd, buffer, sizeof(buffer));
        if (count == -1) {
            if (errno != EAGAIN) {
                perror("read");
                close(client_fd);
                close(clients[client_fd].pg_fd);
                clients.erase(client_fd);
            }
            break;
        } else if (count == 0) {
            close(client_fd);
            close(clients[client_fd].pg_fd);
            clients.erase(client_fd);
            break;
        }

        if (buffer[0] == 'Q') {
            std::string sql_query(buffer + 1, count - 1);
            std::cout << "Received query: " << sql_query << std::endl;
            appendToFile("LogSQL.txt", sql_query);
        }

        int pg_fd = clients[client_fd].pg_fd;
        std::vector<char> data(buffer, buffer + count);
        clients[pg_fd].outbox.push(data);

        epoll_event event;
        event.data.fd = pg_fd;
        event.events = EPOLLOUT | EPOLLIN | EPOLLET;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, pg_fd, &event);
    }
}

void handlePgData(int epoll_fd, int pg_fd, std::map<int, Client>& clients) {
    char buffer[BUFFER_SIZE];
    while (true) {
        ssize_t count = read(pg_fd, buffer, sizeof(buffer));
        if (count == -1) {
            if (errno != EAGAIN) {
                perror("read from pg_fd");
            }
            break;
        } else if (count == 0) {
            close(pg_fd);
            close(clients[pg_fd].fd);
            clients.erase(pg_fd);
            break;
        }

        int client_fd = clients[pg_fd].fd;
        std::vector<char> data(buffer, buffer + count);
        clients[client_fd].outbox.push(data);

        epoll_event event;
        event.data.fd = client_fd;
        event.events = EPOLLOUT | EPOLLIN | EPOLLET;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &event);
    }
}

void handleWrite(int epoll_fd, int fd, std::map<int, Client>& clients) {
    Client& client = clients[fd];
    while (!client.outbox.empty()) {
        std::vector<char>& data = client.outbox.front();
        ssize_t written = write(fd, data.data(), data.size());
        if (written == -1) {
            if (errno != EAGAIN) {
                perror("write");
                close(fd);
                close(client.fd);
                clients.erase(fd);
            }
            break;
        }
        if (written < data.size()) {
            std::vector<char> remaining(data.begin() + written, data.end());
            client.outbox.front() = remaining;
            break;
        }
        client.outbox.pop();
    }

    epoll_event event;
    event.data.fd = fd;
    event.events = (client.outbox.empty() ? EPOLLIN : (EPOLLOUT | EPOLLIN)) | EPOLLET;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
}

int main() {
    int listen_fd = createAndBindSocket(PROXY_PORT);

    if (setNonBlocking(listen_fd) == -1) {
        perror("setNonBlocking");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    epoll_event event;
    event.data.fd = listen_fd;
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1) {
        perror("epoll_ctl: listen_fd");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    std::map<int, Client> clients;
    epoll_event events[MAX_EVENTS];

    while (true) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == listen_fd) {
                handleConnection(epoll_fd, listen_fd, clients);
            } else {
                if (events[i].events & EPOLLIN) {
                    if (clients[fd].fd == fd) {
                        handleClientData(epoll_fd, fd, clients);
                    } else {
                        handlePgData(epoll_fd, fd, clients);
                    }
                }
                if (events[i].events & EPOLLOUT) {
                    handleWrite(epoll_fd, fd, clients);
                }
            }
        }
    }

    close(listen_fd);
    return 0;
}