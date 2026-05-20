#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

int main()
{
    int server_fd;
    int client_socket;

    sockaddr_in address;

    int addrlen = sizeof(address);

    // ============================================
    // Create socket
    // ============================================

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == 0)
    {
        std::cerr << "Socket failed" << std::endl;
        return -1;
    }

    // ============================================
    // Bind
    // ============================================

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(5000);

    if (bind(
            server_fd,
            (sockaddr*)&address,
            sizeof(address)) < 0)
    {
        std::cerr << "Bind failed" << std::endl;
        return -1;
    }

    // ============================================
    // Listen
    // ============================================

    listen(server_fd, 3);

    std::cout
        << "HTTP receiver listening on port 5000..."
        << std::endl;

    while (true)
    {
        client_socket = accept(
            server_fd,
            (sockaddr*)&address,
            (socklen_t*)&addrlen
        );

        if (client_socket < 0)
        {
            std::cerr
                << "Accept failed"
                << std::endl;

            continue;
        }

        std::cout
            << "Client connected"
            << std::endl;

    // ========================================
    // Receive request
    // ========================================

    std::vector<char> requestData;

    char buffer[4096];

    int bytesRead;

    while ((bytesRead =
            recv(
                client_socket,
                buffer,
                sizeof(buffer),
                0)) > 0)
    {
        requestData.insert(
            requestData.end(),
            buffer,
            buffer + bytesRead
        );

        // Check if headers received
        std::string temp(
            requestData.begin(),
            requestData.end()
        );

        size_t headerEnd =
            temp.find("\r\n\r\n");

        if (headerEnd != std::string::npos)
        {
            // Parse content length

            size_t contentLengthPos =
                temp.find("Content-Length:");

            if (contentLengthPos != std::string::npos)
            {
                size_t lineEnd =
                    temp.find(
                        "\r\n",
                        contentLengthPos
                    );

                std::string lengthStr =
                    temp.substr(
                        contentLengthPos + 15,
                        lineEnd - contentLengthPos - 15
                    );

                int contentLength =
                    std::stoi(lengthStr);

                size_t bodyStart =
                    headerEnd + 4;

                size_t bodyBytes =
                    requestData.size() - bodyStart;

                // Stop once full image received

                if (bodyBytes >= contentLength)
                {
                    break;
                }
            }
        }
    }

        // ========================================
        // Find HTTP body
        // ========================================

        std::string requestString(
            requestData.begin(),
            requestData.end()
        );

        size_t bodyPos =
            requestString.find("\r\n\r\n");

        if (bodyPos == std::string::npos)
        {
            std::cerr
                << "Invalid HTTP request"
                << std::endl;

            close(client_socket);

            continue;
        }

        bodyPos += 4;

        // ========================================
        // Save JPEG body
        // ========================================

        std::ofstream file(
            "received.jpg",
            std::ios::binary
        );

        file.write(
            requestData.data() + bodyPos,
            requestData.size() - bodyPos
        );

        file.close();

        std::cout
            << "Saved received.jpg"
            << std::endl;

        // ========================================
        // Send HTTP response
        // ========================================

        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 2\r\n"
            "\r\n"
            "OK";

        send(
            client_socket,
            response.c_str(),
            response.size(),
            0
        );

        close(client_socket);
    }

    close(server_fd);

    return 0;
}