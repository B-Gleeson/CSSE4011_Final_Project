// sender.cpp

#include "sender.hpp"

#include <iostream>
#include <vector>
#include <string>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

// ============================================================
// Base Node Configuration
// ============================================================

static const char* BASE_NODE_IP =
    "192.168.1.54";

static const int BASE_NODE_PORT =
    5001;

// ============================================================
// Send Image
// ============================================================

bool sendImageToBaseNode(
    const cv::Mat& image)
{
    // ========================================================
    // Encode image to JPEG
    // ========================================================

    std::vector<uchar> jpegBuffer;

    bool success =
        cv::imencode(
            ".jpg",
            image,
            jpegBuffer
        );

    if (!success)
    {
        std::cerr
            << "Failed to encode JPEG."
            << std::endl;

        return false;
    }

    // ========================================================
    // Create socket
    // ========================================================

    int sock =
        socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0)
    {
        std::cerr
            << "Socket creation failed."
            << std::endl;

        return false;
    }

    // ========================================================
    // Configure server address
    // ========================================================

    sockaddr_in serverAddr;

    serverAddr.sin_family = AF_INET;

    serverAddr.sin_port =
        htons(BASE_NODE_PORT);

    inet_pton(
        AF_INET,
        BASE_NODE_IP,
        &serverAddr.sin_addr
    );

    // ========================================================
    // Connect to base node
    // ========================================================

    if (connect(
            sock,
            (sockaddr*)&serverAddr,
            sizeof(serverAddr)) < 0)
    {
        std::cerr
            << "Connection to base node failed."
            << std::endl;

        close(sock);

        return false;
    }

    // ========================================================
    // Build HTTP request
    // ========================================================

    std::string headers =
        "POST /upload HTTP/1.1\r\n"
        "Host: " +
        std::string(BASE_NODE_IP) +
        "\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: " +
        std::to_string(jpegBuffer.size()) +
        "\r\n"
        "\r\n";

    // ========================================================
    // Send headers
    // ========================================================

    send(
        sock,
        headers.c_str(),
        headers.size(),
        0
    );

    // ========================================================
    // Send JPEG body
    // ========================================================

    send(
        sock,
        reinterpret_cast<const char*>(
            jpegBuffer.data()
        ),
        jpegBuffer.size(),
        0
    );

    // ========================================================
    // Receive response
    // ========================================================

    char response[1024];

    recv(
        sock,
        response,
        sizeof(response),
        0
    );

    std::cout
        << "Processed image sent to base node."
        << std::endl;

    close(sock);

    return true;
}