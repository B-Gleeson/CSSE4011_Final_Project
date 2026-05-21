#include <arpa/inet.h>
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
#include <vector>

#define SERVER_PORT 5000
#define MAX_IMAGE_SIZE (5 * 1024 * 1024)  // 5 MB

static const std::string SAVE_DIR = "images/incoming";

void make_directories() {
    mkdir("images", 0755);
    mkdir("images/incoming", 0755);
}

std::string make_timestamp_filename() {
    auto now = std::chrono::system_clock::now();
    auto time_now = std::chrono::system_clock::to_time_t(now);

    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()
    ).count() % 1000000;

    struct tm time_info;
    localtime_r(&time_now, &time_info);

    char time_buffer[64];
    strftime(time_buffer, sizeof(time_buffer), "%Y%m%d_%H%M%S", &time_info);

    std::ostringstream filename;
    filename << SAVE_DIR << "/"
             << time_buffer << "_"
             << micros << ".jpg";

    return filename.str();
}

std::string to_lower(const std::string& input) {
    std::string output = input;
    for (char& c : output) {
        c = static_cast<char>(tolower(c));
    }
    return output;
}

int get_content_length(const std::string& headers) {
    std::istringstream stream(headers);
    std::string line;

    while (std::getline(stream, line)) {
        std::string lower_line = to_lower(line);

        std::string key = "content-length:";
        if (lower_line.find(key) == 0) {
            std::string value = line.substr(key.length());

            try {
                return std::stoi(value);
            } catch (...) {
                return -1;
            }
        }
    }

    return -1;
}

void send_response(int client_fd, int status_code, const std::string& status_text,
                   const std::string& body, const std::string& content_type = "text/plain") {
    std::ostringstream response;

    response << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;

    std::string response_str = response.str();
    send(client_fd, response_str.c_str(), response_str.size(), 0);
}

bool save_image_atomic(const std::vector<char>& image_data, std::string& final_path) {
    final_path = make_timestamp_filename();
    std::string temp_path = final_path + ".tmp";

    {
        std::ofstream file(temp_path, std::ios::binary);

        if (!file) {
            std::cerr << "Failed to open file: " << temp_path << std::endl;
            return false;
        }

        file.write(image_data.data(), image_data.size());

        if (!file) {
            std::cerr << "Failed to write image data" << std::endl;
            return false;
        }
    }

    if (rename(temp_path.c_str(), final_path.c_str()) != 0) {
        std::cerr << "Failed to rename temp file: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

bool receive_http_request(int client_fd, std::string& headers, std::vector<char>& body) {
    std::string raw_request;
    char buffer[4096];

    size_t header_end_pos = std::string::npos;

    while (header_end_pos == std::string::npos) {
        ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer), 0);

        if (bytes_received <= 0) {
            return false;
        }

        raw_request.append(buffer, bytes_received);
        header_end_pos = raw_request.find("\r\n\r\n");

        if (raw_request.size() > 8192) {
            std::cerr << "HTTP headers too large" << std::endl;
            return false;
        }
    }

    headers = raw_request.substr(0, header_end_pos + 4);

    int content_length = get_content_length(headers);

    if (content_length < 0) {
        std::cerr << "Missing or invalid Content-Length" << std::endl;
        return false;
    }

    if (content_length > MAX_IMAGE_SIZE) {
        std::cerr << "Image too large: " << content_length << " bytes" << std::endl;
        return false;
    }

    body.clear();
    body.reserve(content_length);

    size_t body_start = header_end_pos + 4;

    if (raw_request.size() > body_start) {
        std::string already_received = raw_request.substr(body_start);
        body.insert(body.end(), already_received.begin(), already_received.end());
    }

    while (body.size() < static_cast<size_t>(content_length)) {
        ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer), 0);

        if (bytes_received <= 0) {
            std::cerr << "Connection closed before full image was received" << std::endl;
            return false;
        }

        body.insert(body.end(), buffer, buffer + bytes_received);
    }

    return true;
}

void handle_client(int client_fd) {
    std::string headers;
    std::vector<char> body;

    if (!receive_http_request(client_fd, headers, body)) {
        send_response(client_fd, 400, "Bad Request", "Bad request\n");
        close(client_fd);
        return;
    }

    if (headers.find("GET / ") == 0 || headers.find("GET / HTTP") == 0) {
        send_response(client_fd, 200, "OK", "Jetson C++ ESP32-CAM receiver running\n");
        close(client_fd);
        return;
    }

    if (headers.find("POST /upload ") != 0 && headers.find("POST /upload HTTP") != 0) {
        send_response(client_fd, 404, "Not Found", "Not found\n");
        close(client_fd);
        return;
    }

    if (body.empty()) {
        send_response(client_fd, 400, "Bad Request", "No image data received\n");
        close(client_fd);
        return;
    }

    std::string saved_path;

    if (!save_image_atomic(body, saved_path)) {
        send_response(client_fd, 500, "Internal Server Error", "Failed to save image\n");
        close(client_fd);
        return;
    }

    std::cout << "Saved " << saved_path
              << " (" << body.size() << " bytes)"
              << std::endl;

    std::ostringstream json;
    json << "{"
         << "\"status\":\"ok\","
         << "\"filename\":\"" << saved_path << "\","
         << "\"bytes\":" << body.size()
         << "}\n";

    send_response(client_fd, 200, "OK", json.str(), "application/json");

    close(client_fd);
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    make_directories();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "Bind failed: " << strerror(errno) << std::endl;
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        std::cerr << "Listen failed" << std::endl;
        close(server_fd);
        return 1;
    }

    std::cout << "Jetson C++ ESP32-CAM receiver running" << std::endl;
    std::cout << "Listening on port " << SERVER_PORT << std::endl;
    std::cout << "Saving images to " << SAVE_DIR << std::endl;

    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(
            server_fd,
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_len
        );

        if (client_fd < 0) {
            std::cerr << "Accept failed" << std::endl;
            continue;
        }

        std::cout << "Connection from "
                  << inet_ntoa(client_addr.sin_addr)
                  << std::endl;

        handle_client(client_fd);
    }

    close(server_fd);
    return 0;
}