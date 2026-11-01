#include <gtest/gtest.h>
#include "server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <string>
#include <chrono>
#include <atomic>

class ServerIntegrationTest : public ::testing::Test {
protected:
    inline static std::atomic<int> next_port_{19100};
    int current_port_ = 19100;
    std::unique_ptr<Server> server;

    void SetUp() override {
        current_port_ = next_port_.fetch_add(1);
        server = std::make_unique<Server>(current_port_, "127.0.0.1", 4);
        ASSERT_TRUE(server->start());
        // Allow event loop to bind and enter epoll_wait
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void TearDown() override {
        if (server) {
            server->stop();
            server.reset();
        }
    }

    int connect_client() {
        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd == -1) return -1;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(current_port_);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
            close(client_fd);
            return -1;
        }
        return client_fd;
    }

    std::string send_and_receive(int client_fd, const std::string& request, bool is_head = false) {
        ssize_t sent = send(client_fd, request.c_str(), request.size(), 0);
        if (sent <= 0) return "";

        std::string response;
        char buffer[4096];
        while (true) {
            ssize_t bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
            if (bytes > 0) {
                buffer[bytes] = '\0';
                response.append(buffer, bytes);
                // Check if we got end of HTTP headers and body or connection closed
                if (response.find("\r\n\r\n") != std::string::npos) {
                    if (is_head) {
                        break;
                    }
                    size_t pos = response.find("Content-Length: ");
                    if (pos != std::string::npos) {
                        size_t end_line = response.find("\r\n", pos);
                        size_t content_len = std::stoul(response.substr(pos + 16, end_line - (pos + 16)));
                        size_t body_start = response.find("\r\n\r\n") + 4;
                        if (response.size() >= body_start + content_len) {
                            break;
                        }
                    } else if (response.find("Connection: close") != std::string::npos) {
                        // Keep reading until EOF
                    }
                }
            } else {
                break; // EOF or error
            }
        }
        return response;
    }
};

// 1. Test Multiple Concurrent Clients
TEST_F(ServerIntegrationTest, MultipleConcurrentClients) {
    const int num_clients = 30;
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < num_clients; ++i) {
        threads.emplace_back([this, &success_count]() {
            int fd = connect_client();
            if (fd >= 0) {
                std::string req = "GET /api/info HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
                std::string resp = send_and_receive(fd, req);
                close(fd);

                if (resp.find("HTTP/1.1 200 OK") != std::string::npos &&
                    resp.find("thread_pool_size") != std::string::npos) {
                    success_count.fetch_add(1);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_clients);
}

// 2. Test HEAD Method Body Suppression & Header Integrity (RFC 7231)
TEST_F(ServerIntegrationTest, HeadMethodCompliance) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);

    std::string req = "HEAD / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    std::string resp = send_and_receive(fd, req, true);
    close(fd);

    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(resp.find("Content-Length: "), std::string::npos);
    EXPECT_NE(resp.find("Content-Type: text/html"), std::string::npos);

    // Verify no body bytes were transmitted after \r\n\r\n
    size_t header_end = resp.find("\r\n\r\n");
    ASSERT_NE(header_end, std::string::npos);
    EXPECT_EQ(resp.size(), header_end + 4);
}

// 3. Test Invalid HTTP Requests (RFC 7230 §5.4 Host requirement and bad syntax)
TEST_F(ServerIntegrationTest, InvalidAndMalformedRequests) {
    // 3a. Complete garbage
    {
        int fd = connect_client();
        ASSERT_GE(fd, 0);
        std::string resp = send_and_receive(fd, "NOT A VALID REQUEST PROTOCOL\r\n\r\n");
        close(fd);
        EXPECT_NE(resp.find("400 Bad Request"), std::string::npos);
    }

    // 3b. Missing Host header in HTTP/1.1 (Mandatory in RFC 7230 §5.4)
    {
        int fd = connect_client();
        ASSERT_GE(fd, 0);
        std::string resp = send_and_receive(fd, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
        close(fd);
        EXPECT_NE(resp.find("400 Bad Request"), std::string::npos);
    }

    // 3c. Unsupported HTTP method (e.g., DELETE)
    {
        int fd = connect_client();
        ASSERT_GE(fd, 0);
        std::string resp = send_and_receive(fd, "DELETE / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        close(fd);
        EXPECT_NE(resp.find("405 Method Not Allowed"), std::string::npos);
    }

    // 3d. Non-existent path returns 404
    {
        int fd = connect_client();
        ASSERT_GE(fd, 0);
        std::string resp = send_and_receive(fd, "GET /this_file_does_not_exist_404.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        close(fd);
        EXPECT_NE(resp.find("404 Not Found"), std::string::npos);
    }
}

// 4. Test Partial Request Transmission (Edge-Triggered Chunk Fragmentation)
TEST_F(ServerIntegrationTest, PartialRequestTransmission) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);

    // Send request in small chunks with delay simulating fragmented network packets
    std::vector<std::string> chunks = {
        "GET /api",
        "/status ",
        "HTTP/1.1\r\n",
        "Host: localhost",
        "\r\nConnection: close",
        "\r\n\r\n"
    };

    for (const auto& chunk : chunks) {
        ssize_t s = send(fd, chunk.c_str(), chunk.size(), 0);
        ASSERT_GT(s, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Server should reconstruct the fragmented request and respond
    std::string response;
    char buffer[1024];
    while (true) {
        ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) {
            response.append(buffer, n);
        } else {
            break;
        }
    }
    close(fd);

    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(response.find("MultithreadedWebServer/1.0"), std::string::npos);
}

// 5. Test Client Disconnection Mid-Request (Abrupt EOF)
TEST_F(ServerIntegrationTest, ClientDisconnectionMidRequest) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);

    // Send partial request and immediately abort connection
    std::string partial = "GET /api/info HTT";
    send(fd, partial.c_str(), partial.size(), 0);
    close(fd); // Immediate RST/FIN

    // Verify server remains fully functional for subsequent requests
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int fd2 = connect_client();
    ASSERT_GE(fd2, 0);

    std::string req = "GET /api/info HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    std::string resp = send_and_receive(fd2, req);
    close(fd2);

    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos);
}

// 6. Test Persistent Connection (HTTP/1.1 Keep-Alive with multiple sequential requests)
TEST_F(ServerIntegrationTest, PersistentConnectionKeepAlive) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);

    // First request: keep-alive
    std::string req1 = "GET /api/info HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    send(fd, req1.c_str(), req1.size(), 0);

    std::string resp1;
    char buf[2048];
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            resp1.append(buf, n);
            if (resp1.find("\r\n\r\n") != std::string::npos) {
                size_t pos = resp1.find("Content-Length: ");
                if (pos != std::string::npos) {
                    size_t end_line = resp1.find("\r\n", pos);
                    size_t len = std::stoul(resp1.substr(pos + 16, end_line - (pos + 16)));
                    size_t body_start = resp1.find("\r\n\r\n") + 4;
                    if (resp1.size() >= body_start + len) break;
                }
            }
        } else {
            break;
        }
    }

    EXPECT_NE(resp1.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(resp1.find("Connection: keep-alive"), std::string::npos);

    // Second request on the SAME socket: close
    std::string req2 = "GET /api/status HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send(fd, req2.c_str(), req2.size(), 0);

    std::string resp2;
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            resp2.append(buf, n);
        } else {
            break;
        }
    }
    close(fd);

    EXPECT_NE(resp2.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(resp2.find("Connection: close"), std::string::npos);
}

// 7. Test Graceful Server Shutdown
TEST_F(ServerIntegrationTest, GracefulShutdownUnderLoad) {
    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> workers;

    for (int i = 0; i < 5; ++i) {
        workers.emplace_back([this, &stop_flag]() {
            while (!stop_flag.load()) {
                int fd = connect_client();
                if (fd >= 0) {
                    std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
                    send_and_receive(fd, req);
                    close(fd);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }

    // Let clients hammer server for 100ms
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Call graceful shutdown while requests are actively in flight
    server->stop();
    EXPECT_FALSE(server->is_running());

    stop_flag.store(true);
    for (auto& w : workers) {
        w.join();
    }
}
