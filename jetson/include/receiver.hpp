#ifndef SERVER_HPP
#define SERVER_HPP

#include <string>
#include <vector>

#include <netinet/in.h>

#define SERVER_PORT 5000
#define MAX_IMAGE_SIZE (5 * 1024 * 1024)
#define MAX_HEADER_SIZE 8192

// ============================================================
// Global State
// ============================================================

extern const std::string SAVE_DIR;

extern std::string latest_image_path;
extern int latest_frame_id;

extern bool latest_ppe_detected;
extern std::string latest_missing_item;
extern double latest_confidence;
extern std::string latest_action;

// ============================================================
// Utilities
// ============================================================

void make_directories();

bool ends_with(
    const std::string& s,
    const std::string& suffix
);

void load_latest_existing_image();

std::string make_timestamp_filename();

std::string to_lower(
    const std::string& input
);

int get_content_length(
    const std::string& headers
);

bool parse_request_line(
    const std::string& headers,
    std::string& method,
    std::string& path
);

// ============================================================
// HTTP Response Helpers
// ============================================================

void send_response(
    int client_fd,
    int status_code,
    const std::string& status_text,
    const std::string& body,
    const std::string& content_type = "text/plain"
);

void send_json_response(
    int client_fd,
    int status_code,
    const std::string& status_text,
    const std::string& body
);

bool read_file_binary(
    const std::string& path,
    std::string& out
);

// ============================================================
// Image Saving
// ============================================================

bool save_image_atomic(
    const std::vector<char>& image_data,
    std::string& final_path
);

// ============================================================
// HTTP Request Receive
// ============================================================

bool receive_http_request(
    int client_fd,
    std::string& method,
    std::string& path,
    std::string& headers,
    std::vector<char>& body
);

// ============================================================
// Endpoint Handlers
// ============================================================

void handle_home(
    int client_fd
);

void handle_health(
    int client_fd
);

void handle_latest_result(
    int client_fd
);

void handle_latest_image(
    int client_fd
);

void handle_take_photo_command(
    int client_fd
);

void handle_upload(
    int client_fd,
    const std::vector<char>& body
);

// ============================================================
// Client Dispatcher
// ============================================================

void handle_client(
    int client_fd
);

#endif