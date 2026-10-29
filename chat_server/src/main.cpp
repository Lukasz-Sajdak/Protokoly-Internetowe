#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>

// Wrapper RAII dla gniazd
class FileDescriptor {
    int fd_;
public:
    explicit FileDescriptor(int fd = -1) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    int get() const { return fd_; }
    bool is_valid() const { return fd_ >= 0; }
};

enum class ClientStatus { CONNECTED, AUTHENTICATED };

// Stan sesji klienta
struct ClientContext {
    int fd;
    ClientStatus status = ClientStatus::CONNECTED;
    std::string id;
    std::string buffer;
    
    std::chrono::steady_clock::time_point connection_time;
    std::chrono::steady_clock::time_point last_activity;
    bool ping_sent = false;
    std::chrono::steady_clock::time_point ping_sent_time;
};

class ChatServer {
private:
    FileDescriptor server_fd, epoll_fd;
    std::unordered_map<int, ClientContext> clients;
    std::unordered_map<std::string, int> active_ids;
    static constexpr int MAX_EVENTS = 64;
    static constexpr uint16_t SERVER_PORT = 9000;

    void set_nonblocking(int fd) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    }

    void send_to_client(int fd, const std::string& msg) {
        std::string final_msg = msg + "\n";
        send(fd, final_msg.c_str(), final_msg.length(), 0);
    }

    void disconnect_client(int fd) {
        auto it = clients.find(fd);
        if (it != clients.end()) {
            if (it->second.status == ClientStatus::AUTHENTICATED) {
                active_ids.erase(it->second.id);
            }
            clients.erase(it);
        }
        close(fd);
    }

    // Nowe połączenie i inicjacja WHO
    void handle_new_connection() {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd.get(), (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) return;

        set_nonblocking(client_fd);
        struct epoll_event event{};
        event.events = EPOLLIN; 
        event.data.fd = client_fd;
        epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, client_fd, &event);

        auto now = std::chrono::steady_clock::now();
        clients[client_fd] = ClientContext{
            client_fd, ClientStatus::CONNECTED, "", "", now, now, false, now
        };
        
        send_to_client(client_fd, "WHO");
    }

    // Parsowanie i logika STMP
    void process_command(ClientContext& ctx, const std::string& line) {
        ctx.last_activity = std::chrono::steady_clock::now();
        ctx.ping_sent = false;

        if (line.empty()) return;

        size_t space_pos = line.find(' ');
        std::string cmd = line.substr(0, space_pos);
        std::string args = (space_pos != std::string::npos) ? line.substr(space_pos + 1) : "";

        if (ctx.status == ClientStatus::CONNECTED) {
            if (cmd == "NAME") {
                if (args.length() != 8) {
                    send_to_client(ctx.fd, "ERR_INVALIDNAME");
                    return;
                }
                if (active_ids.find(args) != active_ids.end()) {
                    send_to_client(ctx.fd, "ERR NAMETAKEN");
                    return;
                }
                ctx.id = args;
                ctx.status = ClientStatus::AUTHENTICATED;
                active_ids[args] = ctx.fd;
                send_to_client(ctx.fd, "OK");
            } else if (cmd == "QUIT") {
                disconnect_client(ctx.fd);
            } else {
                send_to_client(ctx.fd, "ERR NOTREGISTERED");
            }
            return;
        }

        if (cmd == "MESG") {
            if (args.empty()) {
                send_to_client(ctx.fd, "ERR BADARGS");
                return;
            }
            size_t arg_space = args.find(' ');
            if (arg_space == std::string::npos) {
                send_to_client(ctx.fd, "ERR BADARGS");
                return;
            }
            
            std::string target_id = args.substr(0, arg_space);
            std::string msg_content = args.substr(arg_space + 1);

            if (msg_content.length() > 1024) {
                send_to_client(ctx.fd, "ERR TOOLONG");
                return;
            }
            if (target_id == ctx.id) {
                send_to_client(ctx.fd, "ERR BADTARGET");
                return;
            }
            
            auto it = active_ids.find(target_id);
            if (it == active_ids.end()) {
                send_to_client(ctx.fd, "ERR NOUSER");
                return;
            }

            send_to_client(it->second, "FROM " + ctx.id + " " + msg_content);
            send_to_client(ctx.fd, "OK");

        } else if (cmd == "LIST") {
            std::string list_response = "OK";
            for (const auto& pair : active_ids) {
                list_response += "\n" + pair.first;
            }
            send_to_client(ctx.fd, list_response);

        } else if (cmd == "QUIT") {
            disconnect_client(ctx.fd);

        } else if (cmd == "PONG") {
            // Zresetowano wyżej
        } else {
            send_to_client(ctx.fd, "ERR UNKNOWNCMD");
        }
    }

    // Czytanie z bufora TCP
    void handle_client_data(int fd) {
        auto it = clients.find(fd);
        if (it == clients.end()) return;
        ClientContext& ctx = it->second;

        char temp_buf[1024];
        ssize_t bytes_read = recv(fd, temp_buf, sizeof(temp_buf), 0);

        if (bytes_read <= 0) {
            disconnect_client(fd);
            return;
        }

        ctx.buffer.append(temp_buf, bytes_read);
        size_t pos;
        
        while ((pos = ctx.buffer.find('\n')) != std::string::npos) {
            std::string line = ctx.buffer.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) process_command(ctx, line);
            
            ctx.buffer.erase(0, pos + 1);
            if (clients.find(fd) == clients.end()) break; 
        }
    }

    // Obsługa timeoutów STMP
    void handle_timers() {
        auto now = std::chrono::steady_clock::now();
        std::vector<int> to_disconnect;

        for (auto& [fd, ctx] : clients) {
            if (ctx.status == ClientStatus::CONNECTED) {
                if (std::chrono::duration_cast<std::chrono::seconds>(now - ctx.connection_time).count() > 5) {
                    send_to_client(fd, "ERR TIMEOUT");
                    to_disconnect.push_back(fd);
                }
            } else if (ctx.status == ClientStatus::AUTHENTICATED) {
                if (!ctx.ping_sent) {
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - ctx.last_activity).count() > 60) {
                        send_to_client(fd, "PING");
                        ctx.ping_sent = true;
                        ctx.ping_sent_time = now;
                    }
                } else {
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - ctx.ping_sent_time).count() > 10) {
                        send_to_client(fd, "ERR TIMEOUT");
                        to_disconnect.push_back(fd);
                    }
                }
            }
        }

        for (int fd : to_disconnect) {
            disconnect_client(fd);
        }
    }

public:
    ChatServer() {
        server_fd = FileDescriptor(socket(AF_INET, SOCK_STREAM, 0));
        if (!server_fd.is_valid()) throw std::runtime_error("Failed to create socket");

        int opt = 1;
        setsockopt(server_fd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        set_nonblocking(server_fd.get());

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(SERVER_PORT);

        if (bind(server_fd.get(), (struct sockaddr*)&address, sizeof(address)) < 0)
            throw std::runtime_error("Bind failed");
        if (listen(server_fd.get(), SOMAXCONN) < 0)
            throw std::runtime_error("Listen failed");

        epoll_fd = FileDescriptor(epoll_create1(0));
        struct epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = server_fd.get();
        epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, server_fd.get(), &event);

        std::cout << "STMP Chat server started on port " << SERVER_PORT << ".\n";
    }

    // Główna pętla zdarzeń
    void run() {
        std::vector<struct epoll_event> events(MAX_EVENTS);
        while (true) {
            int num_events = epoll_wait(epoll_fd.get(), events.data(), MAX_EVENTS, 1000);
            
            for (int i = 0; i < num_events; ++i) {
                if (events[i].data.fd == server_fd.get()) handle_new_connection();
                else handle_client_data(events[i].data.fd);
            }
            
            handle_timers();
        }
    }
};

int main() {
    try {
        ChatServer server;
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}