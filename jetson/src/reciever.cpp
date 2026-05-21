// receiver.cpp

#include "receiver.hpp"

#include <iostream>
#include <fstream>
#include <vector>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

// ============================================================
// Receive Image
// ============================================================

bool receiveImage(
    const std::string& savePath)
{
    // ========================================================
    // Create socket
    // ========================================================

    int server_fd =
        socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        std::cerr
            << "Socket failed."
            << std::endl;

        return false;
    }

    int opt = 1;

    setsockopt(
        server_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &opt,
        sizeof(opt)
    );

    // ========================================================
    // Configure address
    // ========================================================

    sockaddr_in address;

    address.sin_family = AF_INET;

    address.sin_addr.s_addr = INADDR_ANY;

    address.sin_port = htons(5000);

    // ========================================================
    // Bind
    // ========================================================

    if (bind(
            server_fd,
            (sockaddr*)&address,
            sizeof(address)) < 0)
    {
        std::cerr
            << "Bind failed."
            << std::endl;

        close(server_fd);

        return false;
    }

    // ========================================================
    // Listen
    // ========================================================

    listen(server_fd, 1);

    std::cout
        << "Waiting for ESP32 image..."
        << std::endl;

    // ========================================================
    // Accept connection
    // ========================================================

    int addrlen = sizeof(address);

    int client_socket =
        accept(
            server_fd,
            (sockaddr*)&address,
            (socklen_t*)&addrlen
        );

    if (client_socket < 0)
    {
        std::cerr
            << "Accept failed."
            << std::endl;

        close(server_fd);

        return false;
    }

    // ========================================================
    // Receive request
    // ========================================================

    std::vector<char> requestData;

    char buffer[4096];

    int bytesRead;

    int contentLength = -1;

    size_t headerEnd =
        std::string::npos;

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

        std::string temp(
            requestData.begin(),
            requestData.end()
        );

        headerEnd =
            temp.find("\r\n\r\n");

        if (headerEnd == std::string::npos)
        {
            continue;
        }

        if (contentLength < 0)
        {
            size_t pos =
                temp.find(
                    "Content-Length:"
                );

            if (pos != std::string::npos)
            {
                size_t lineEnd =
                    temp.find("\r\n", pos);

                std::string lenStr =
                    temp.substr(
                        pos + 15,
                        lineEnd - pos - 15
                    );

                contentLength =
                    std::stoi(lenStr);
            }
        }

        if (contentLength > 0)
        {
            size_t bodyStart =
                headerEnd + 4;

            size_t bodyBytes =
                requestData.size() -
                bodyStart;

            if (bodyBytes >=
                (size_t)contentLength)
            {
                break;
            }
        }
    }

    // ========================================================
    // Save image
    // ========================================================

    size_t bodyPos =
        headerEnd + 4;

    std::ofstream file(
        savePath,
        std::ios::binary
    );

    file.write(
        requestData.data() + bodyPos,
        contentLength
    );

    file.close();

    std::cout
        << "Saved: "
        << savePath
        << std::endl;

    // ========================================================
    // HTTP response
    // ========================================================

    std::string response =
        "HTTP/1.1 200 OK\r\n"
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

    close(server_fd);

    return true;
}