// main.cpp

#include <iostream>
#include <chrono>

#include <opencv2/opencv.hpp>

#include "detector.hpp"
#include "camera.hpp"
#include "alert_logic.hpp"
#include "render.hpp"

int main(int argc, char** argv)
{
    // ============================================================
    // Check arguments
    // ============================================================

    if (argc < 2)
    {
        std::cout << "Usage: ./app <engine_file>"
                  << std::endl;

        return -1;
    }

    std::string enginePath = argv[1];

    // ============================================================
    // Initialize systems
    // ============================================================

    YOLODetector detector(enginePath);

    CameraReceiver camera;

    Renderer renderer;

    AlertLogic alerts;

    // ============================================================
    // Connect MQTT camera receiver
    // ============================================================

    if (!camera.connect())
    {
        std::cerr << "Failed to connect camera receiver."
                  << std::endl;

        return -1;
    }

    std::cout << "System initialized."
              << std::endl;

    // ============================================================
    // Main loop
    // ============================================================

    cv::Mat frame;

    while (true)
    {
        // ========================================================
        // Receive frame from ESP32-CAM
        // ========================================================

        bool success = camera.getFrame(frame);

        if (!success)
        {
            continue;
        }

        // ========================================================
        // Start timing
        // ========================================================

        auto start =
            std::chrono::high_resolution_clock::now();

        // ========================================================
        // Run inference
        // ========================================================

        std::vector<Detection> detections =
            detector.infer(frame);

        // ========================================================
        // Check alerts
        // ========================================================

        bool violation =
            alerts.hasViolation(detections);

        if (violation)
        {
            alerts.triggerAlert();

            alerts.drawAlertOverlay(frame);
        }

        // ========================================================
        // Draw detections
        // ========================================================

        renderer.drawDetections(
            frame,
            detections
        );

        // ========================================================
        // Calculate FPS
        // ========================================================

        auto end =
            std::chrono::high_resolution_clock::now();

        float fps =
            1000.0f /
            std::chrono::duration<float, std::milli>(
                end - start
            ).count();

        renderer.drawFPS(frame, fps);

        // ========================================================
        // Display frame
        // ========================================================

        renderer.show(
            "Hardhat Detection",
            frame
        );

        // ========================================================
        // Quit handling
        // ========================================================

        int key = cv::waitKey(1);

        if (key == 'q' || key == 27)
        {
            break;
        }
    }

    // ============================================================
    // Cleanup
    // ============================================================

    camera.disconnect();

    cv::destroyAllWindows();

    std::cout << "Shutdown complete."
              << std::endl;

    return 0;
}