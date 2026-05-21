// telemetry.cpp

#include "telemetry.hpp"

#include <iostream>
#include <sstream>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

// ============================================================
// Base Node
// ============================================================

static const char* BASE_NODE_IP =
    "192.168.1.100";

static const int BASE_NODE_PORT =
    5002;

// ============================================================
// Send Telemetry
// ============================================================

bool sendTelemetry(
    int frameID,
    const std::string& nodeID,
    bool ppeDetected,
    const std::vector<std::string>& missingItems,
    float confidence,
    const std::string& action)
{
    // ========================================================
    // Build JSON
    // ========================================================

    std::stringstream json;

    json << "{";
    json << "\"frame_id\":" << frameID << ",";
    json << "\"node_id\":\"" << nodeID << "\",";
    json << "\"ppe_detected\":"
         << (ppeDetected ? "true" : "false")
         << ",";

    json << "\"missing_items\":[";

    for (size_t i = 0;
         i < missingItems.size();
         i++)
    {
        json << "\""
             << missingItems[i]
             << "\"";

        if (i != missingItems.size() - 1)
        {
            json << ",";
        }
    }

    json << "],";

    json << "\"confidence\":"
         << confidence
         << ",";

    json << "\"action\":\""
         << action
         << "\"";

    json << "}";

    std::string jsonBody =
        json.str();

    // ========================================================
    // Create socket
    // ========================================================

    int sock =
        socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0)
    {
        std::cerr
            << "Telemetry socket failed."
            << std::endl;

        return false;
    }

    // ========================================================
    // Configure address
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
    // Connect
    // ========================================================

    if (connect(
            sock,
            (sockaddr*)&serverAddr,
            sizeof(serverAddr)) < 0)
    {
        std::cerr
            << "Telemetry connection failed."
            << std::endl;

        close(sock);

        return false;
    }

    // ========================================================
    // Build HTTP request
    // ========================================================

    std::stringstream request;

    request
        << "POST /telemetry HTTP/1.1\r\n"
        << "Host: "
        << BASE_NODE_IP
        << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: "
        << jsonBody.size()
        << "\r\n"
        << "\r\n"
        << jsonBody;

    std::string requestStr =
        request.str();

    // ========================================================
    // Send
    // ========================================================

    send(
        sock,
        requestStr.c_str(),
        requestStr.size(),
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
        << "Telemetry sent."
        << std::endl;

    close(sock);

    return true;
}