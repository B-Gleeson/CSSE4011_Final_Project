#include "receiver.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ============================================================
// Configuration
// ============================================================

#define SERVER_PORT 5000
#define MAX_IMAGE_SIZE (5 * 1024 * 1024)
#define MAX_HEADER_SIZE 8192

const std::string SAVE_DIR =
    "receiver/images/incoming";

// ============================================================
// Global State
// ============================================================

std::string latest_image_path;

int latest_frame_id = 0;

bool latest_ppe_detected = false;

std::string latest_missing_item =
    "hardhat";

double latest_confidence = 0.91;

std::string latest_action =
    "alert";

// ============================================================
// Utilities
// ============================================================

void make_directories()
{
    mkdir("receiver", 0755);

    mkdir("receiver/images", 0755);

    mkdir("receiver/images/incoming", 0755);
}

bool ends_with(
    const std::string& s,
    const std::string& suffix)
{
    if (s.size() < suffix.size())
    {
        return false;
    }

    return s.compare(
        s.size() - suffix.size(),
        suffix.size(),
        suffix
    ) == 0;
}

void load_latest_existing_image()
{
    DIR* dir =
        opendir(SAVE_DIR.c_str());

    if (!dir)
    {
        return;
    }

    std::string latest_name;

    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL)
    {
        std::string name =
            entry->d_name;

        if (ends_with(name, ".jpg"))
        {
            if (latest_name.empty() ||
                name > latest_name)
            {
                latest_name = name;
            }
        }
    }

    closedir(dir);

    if (!latest_name.empty())
    {
        latest_image_path =
            SAVE_DIR + "/" + latest_name;

        std::cout
            << "Loaded latest image: "
            << latest_image_path
            << std::endl;
    }
}

std::string make_timestamp_filename()
{
    auto now =
        std::chrono::system_clock::now();

    auto time_now =
        std::chrono::system_clock::to_time_t(now);

    auto micros =
        std::chrono::duration_cast<
            std::chrono::microseconds>(
                now.time_since_epoch()
        ).count() % 1000000;

    struct tm time_info;

    localtime_r(
        &time_now,
        &time_info
    );

    char time_buffer[64];

    strftime(
        time_buffer,
        sizeof(time_buffer),
        "%Y%m%d_%H%M%S",
        &time_info
    );

    std::ostringstream filename;

    filename
        << SAVE_DIR
        << "/"
        << time_buffer
        << "_"
        << micros
        << ".jpg";

    return filename.str();
}

std::string to_lower(
    const std::string& input)
{
    std::string output = input;

    for (char& c : output)
    {
        c = static_cast<char>(
            tolower(c)
        );
    }

    return output;
}

int get_content_length(
    const std::string& headers)
{
    std::istringstream stream(headers);

    std::string line;

    while (std::getline(stream, line))
    {
        std::string lower_line =
            to_lower(line);

        std::string key =
            "content-length:";

        if (lower_line.find(key) == 0)
        {
            std::string value =
                line.substr(key.length());

            try
            {
                return std::stoi(value);
            }
            catch (...)
            {
                return -1;
            }
        }
    }

    return 0;
}

bool parse_request_line(
    const std::string& headers,
    std::string& method,
    std::string& path)
{
    std::istringstream stream(headers);

    std::string request_line;

    if (!std::getline(stream, request_line))
    {
        return false;
    }

    if (!request_line.empty() &&
        request_line.back() == '\r')
    {
        request_line.pop_back();
    }

    std::istringstream line_stream(
        request_line
    );

    std::string version;

    line_stream
        >> method
        >> path
        >> version;

    return !method.empty() &&
           !path.empty();
}

// ============================================================
// HTTP Helpers
// ============================================================

void send_response(
    int client_fd,
    int status_code,
    const std::string& status_text,
    const std::string& body,
    const std::string& content_type)
{
    std::ostringstream header;

    header
        << "HTTP/1.1 "
        << status_code
        << " "
        << status_text
        << "\r\n";

    header
        << "Content-Type: "
        << content_type
        << "\r\n";

    header
        << "Content-Length: "
        << body.size()
        << "\r\n";

    header
        << "Connection: close\r\n";

    header << "\r\n";

    std::string header_str =
        header.str();

    send(
        client_fd,
        header_str.data(),
        header_str.size(),
        0
    );

    if (!body.empty())
    {
        send(
            client_fd,
            body.data(),
            body.size(),
            0
        );
    }
}

void send_json_response(
    int client_fd,
    int status_code,
    const std::string& status_text,
    const std::string& body)
{
    send_response(
        client_fd,
        status_code,
        status_text,
        body,
        "application/json"
    );
}

// ============================================================
// Image Saving
// ============================================================

bool save_image_atomic(
    const std::vector<char>& image_data,
    std::string& final_path)
{
    final_path =
        make_timestamp_filename();

    std::string temp_path =
        final_path + ".tmp";

    {
        std::ofstream file(
            temp_path,
            std::ios::binary
        );

        if (!file)
        {
            return false;
        }

        file.write(
            image_data.data(),
            image_data.size()
        );
    }

    if (rename(
            temp_path.c_str(),
            final_path.c_str()) != 0)
    {
        return false;
    }

    latest_image_path =
        final_path;

    latest_frame_id++;

    return true;
}

// ============================================================
// Receive HTTP Request
// ============================================================

bool receive_http_request(
    int client_fd,
    std::string& method,
    std::string& path,
    std::string& headers,
    std::vector<char>& body)
{
    std::string raw_request;

    char buffer[4096];

    size_t header_end_pos =
        std::string::npos;

    while (header_end_pos ==
           std::string::npos)
    {
        ssize_t bytes_received =
            recv(
                client_fd,
                buffer,
                sizeof(buffer),
                0
            );

        if (bytes_received <= 0)
        {
            return false;
        }

        raw_request.append(
            buffer,
            bytes_received
        );

        header_end_pos =
            raw_request.find("\r\n\r\n");

        if (raw_request.size() >
            MAX_HEADER_SIZE)
        {
            return false;
        }
    }

    headers =
        raw_request.substr(
            0,
            header_end_pos + 4
        );

    if (!parse_request_line(
            headers,
            method,
            path))
    {
        return false;
    }

    int content_length =
        get_content_length(headers);

    if (content_length < 0 ||
        content_length >
        MAX_IMAGE_SIZE)
    {
        return false;
    }

    body.clear();

    size_t body_start =
        header_end_pos + 4;

    if (raw_request.size() > body_start)
    {
        body.insert(
            body.end(),
            raw_request.begin() + body_start,
            raw_request.end()
        );
    }

    while (body.size() <
           static_cast<size_t>(
               content_length))
    {
        ssize_t bytes_received =
            recv(
                client_fd,
                buffer,
                sizeof(buffer),
                0
            );

        if (bytes_received <= 0)
        {
            return false;
        }

        body.insert(
            body.end(),
            buffer,
            buffer + bytes_received
        );
    }

    return true;
}

// ============================================================
// Upload Handler
// ============================================================

void handle_upload(
    int client_fd,
    const std::vector<char>& body)
{
    if (body.empty())
    {
        send_response(
            client_fd,
            400,
            "Bad Request",
            "No image data\n",
            "text/plain"
        );

        return;
    }

    std::string saved_path;

    if (!save_image_atomic(
            body,
            saved_path))
    {
        send_response(
            client_fd,
            500,
            "Internal Server Error",
            "Save failed\n",
            "text/plain"
        );

        return;
    }

    std::cout
        << "Saved "
        << saved_path
        << std::endl;

    send_json_response(
        client_fd,
        200,
        "OK",
        "{\"status\":\"ok\"}\n"
    );
}

// ============================================================
// Client Handler
// ============================================================

void handle_client(
    int client_fd)
{
    std::string method;
    std::string path;
    std::string headers;

    std::vector<char> body;

    if (!receive_http_request(
            client_fd,
            method,
            path,
            headers,
            body))
    {
        close(client_fd);
        return;
    }

    if (method == "POST" &&
        path == "/upload")
    {
        handle_upload(
            client_fd,
            body
        );

        close(client_fd);
        return;
    }

    send_response(
        client_fd,
        404,
        "Not Found",
        "Not Found\n",
        "text/plain"
    );

    close(client_fd);
}

// ============================================================
// Receiver Thread
// ============================================================

void start_receiver_server()
{
    signal(SIGPIPE, SIG_IGN);

    make_directories();

    load_latest_existing_image();

    int server_fd =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (server_fd < 0)
    {
        std::cerr
            << "Socket creation failed"
            << std::endl;

        return;
    }

    int reuse = 1;

    setsockopt(
        server_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );

    sockaddr_in server_addr;

    memset(
        &server_addr,
        0,
        sizeof(server_addr)
    );

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_addr.s_addr =
        INADDR_ANY;

    server_addr.sin_port =
        htons(SERVER_PORT);

    if (bind(
            server_fd,
            reinterpret_cast<
                sockaddr*>(&server_addr),
            sizeof(server_addr)) < 0)
    {
        std::cerr
            << "Bind failed"
            << std::endl;

        close(server_fd);

        return;
    }

    if (listen(server_fd, 5) < 0)
    {
        std::cerr
            << "Listen failed"
            << std::endl;

        close(server_fd);

        return;
    }

    std::cout
        << "Receiver server running on port "
        << SERVER_PORT
        << std::endl;

    while (true)
    {
        sockaddr_in client_addr;

        socklen_t client_len =
            sizeof(client_addr);

        int client_fd =
            accept(
                server_fd,
                reinterpret_cast<
                    sockaddr*>(&client_addr),
                &client_len
            );

        if (client_fd < 0)
        {
            continue;
        }

        handle_client(client_fd);
    }
}