#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

class FileDescriptor {
public:
    explicit FileDescriptor(int fd = -1) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) close(fd_); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const { return fd_; }
    bool is_valid() const { return fd_ >= 0; }

private:
    int fd_;
};

int main() {
    constexpr uint16_t SERVER_PORT = 5000;
    constexpr int MAX_QUEUED_CONNECTIONS = 3;

    FileDescriptor server_socket(socket(AF_INET, SOCK_STREAM, 0));
    if (!server_socket.is_valid()) {
        std::cerr << "Error: Socket creation failed.\n";
        return 1;
    }

    int reuse_port_option = 1;
    setsockopt(server_socket.get(), SOL_SOCKET, SO_REUSEADDR, &reuse_port_option, sizeof(reuse_port_option));

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(SERVER_PORT);

    if (bind(server_socket.get(), reinterpret_cast<struct sockaddr*>(&server_address), sizeof(server_address)) < 0) {
        std::cerr << "Error: Bind failed.\n";
        return 1;
    }

    if (listen(server_socket.get(), MAX_QUEUED_CONNECTIONS) < 0) {
        std::cerr << "Error: Listen failed.\n";
        return 1;
    }

    std::cout << "Server listening on port " << SERVER_PORT << "...\n";

    sockaddr_in client_address{};
    socklen_t client_address_len = sizeof(client_address);
    
    FileDescriptor client_socket(accept(server_socket.get(), reinterpret_cast<struct sockaddr*>(&client_address), &client_address_len));
    
    if (client_socket.is_valid()) {
        std::cout << "Client connected.\n";
        
        constexpr const char* initial_prompt = "WHO\n";
        send(client_socket.get(), initial_prompt, strlen(initial_prompt), 0);
    }

    return 0;
}