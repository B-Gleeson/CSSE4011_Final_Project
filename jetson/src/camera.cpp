// camera.cpp
// Jetson MQTT receiver from BASE NODE

#include "camera.hpp"

#include <iostream>
#include <vector>

#include <opencv2/opencv.hpp>

#include <mqtt/async_client.h>

// ============================================================
// MQTT Configuration
// ============================================================

// MQTT broker running on base node
const std::string SERVER_ADDRESS =
    "tcp://192.168.1.100:1883";

// Jetson MQTT client ID
const std::string CLIENT_ID =
    "jetson_inference_node";

// Base node publishes frames here
const std::string FRAME_TOPIC =
    "base/frame";

// ============================================================
// Constructor
// ============================================================

CameraReceiver::CameraReceiver()
    : client(SERVER_ADDRESS, CLIENT_ID)
{
}

// ============================================================
// Connect to MQTT broker
// ============================================================

bool CameraReceiver::connect()
{
    try
    {
        mqtt::connect_options connOpts;

        connOpts.set_clean_session(true);

        std::cout
            << "Connecting to base node MQTT broker..."
            << std::endl;

        client.connect(connOpts)->wait();

        std::cout
            << "Connected to MQTT broker."
            << std::endl;

        // Enable message queue
        client.start_consuming();

        // Subscribe to incoming image stream
        client.subscribe(FRAME_TOPIC, 1)->wait();

        std::cout
            << "Subscribed to topic: "
            << FRAME_TOPIC
            << std::endl;

        return true;
    }
    catch (const mqtt::exception& e)
    {
        std::cerr
            << "MQTT connection error: "
            << e.what()
            << std::endl;

        return false;
    }
}

// ============================================================
// Receive frame from base node
// ============================================================

bool CameraReceiver::getFrame(cv::Mat& frame)
{
    // ============================================
    // Wait for MQTT message
    // ============================================

    auto msg = client.consume_message();

    if (!msg)
    {
        return false;
    }

    // ============================================
    // Extract JPEG payload
    // ============================================

    auto payload = msg->get_payload();

    if (payload.empty())
    {
        std::cerr
            << "Received empty payload."
            << std::endl;

        return false;
    }

    // ============================================
    // Convert payload -> JPEG byte vector
    // ============================================

    std::vector<uchar> jpegData(
        payload.begin(),
        payload.end()
    );

    // ============================================
    // Decode JPEG into OpenCV image
    // ============================================

    frame = cv::imdecode(
        jpegData,
        cv::IMREAD_COLOR
    );

    if (frame.empty())
    {
        std::cerr
            << "Failed to decode incoming frame."
            << std::endl;

        return false;
    }

    return true;
}

// ============================================================
// Disconnect from MQTT broker
// ============================================================

void CameraReceiver::disconnect()
{
    try
    {
        client.unsubscribe(FRAME_TOPIC)->wait();

        client.stop_consuming();

        client.disconnect()->wait();

        std::cout
            << "Disconnected from MQTT broker."
            << std::endl;
    }
    catch (const mqtt::exception& e)
    {
        std::cerr
            << "MQTT disconnect error: "
            << e.what()
            << std::endl;
    }
}