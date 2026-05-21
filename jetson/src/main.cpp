// main.cpp

#include "detector.hpp"
#include "sender.hpp"
#include "receiver.hpp"
#include "telemetry.hpp"

#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>

#include <opencv2/opencv.hpp>

#include <experimental/filesystem>

namespace fs = std::experimental::filesystem;

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv)
{
    // ========================================================
    // Check arguments
    // ========================================================

    if (argc < 2)
    {
        std::cout
            << "Usage: ./ppe_inference engine"
            << std::endl;

        return -1;
    }

    // ========================================================
    // Engine path
    // ========================================================

    std::string enginePath =
        argv[1];

    // ========================================================
    // Directories
    // ========================================================

    std::string incomingDir =
        "receiver/images/incoming";

    std::string outputDir =
        "receiver/images/output";

    // ========================================================
    // Create directories
    // ========================================================

    fs::create_directories(incomingDir);

    fs::create_directories(outputDir);

    // ========================================================
    // Create detector
    // ========================================================

    YOLODetector detector(enginePath);

    // ========================================================
    // Frame counter
    // ========================================================

    int frameID = 0;

    std::cout
        << "PPE inference system running."
        << std::endl;

    // ========================================================
    // Main loop
    // ========================================================

    while (true)
    {
        // ====================================================
        // Receive image from ESP32
        // ====================================================

        std::string receivedImagePath =
            incomingDir + "/latest.jpg";

        bool received =
            receiveImage(receivedImagePath);

        if (!received)
        {
            std::cout
                << "Failed to receive image."
                << std::endl;

            continue;
        }

        // ====================================================
        // Small delay
        // ====================================================

        std::this_thread::sleep_for(
            std::chrono::milliseconds(100)
        );

        // ====================================================
        // Load image
        // ====================================================

        cv::Mat image =
            cv::imread(receivedImagePath);

        if (image.empty())
        {
            std::cout
                << "Failed to load image."
                << std::endl;

            continue;
        }

        // ====================================================
        // Run inference
        // ====================================================

        std::vector<Detection>
            detections =
                detector.infer(image);

        // ====================================================
        // PPE analysis
        // ====================================================

        bool helmetFound = false;

        float bestConfidence = 0.0f;

        std::vector<std::string>
            missingItems;

        for (const auto& det : detections)
        {
            if (det.class_id == 1)
            {
                helmetFound = true;

                if (det.confidence >
                    bestConfidence)
                {
                    bestConfidence =
                        det.confidence;
                }
            }
        }

        if (!helmetFound)
        {
            missingItems.push_back(
                "hardhat"
            );
        }

        // ====================================================
        // Draw detections
        // ====================================================

        for (const auto& det : detections)
        {
            // =================================================
            // Clamp box
            // =================================================

            int x =
                std::max(0, det.box.x);

            int y =
                std::max(0, det.box.y);

            int width =
                std::min(
                    det.box.width,
                    image.cols - x
                );

            int height =
                std::min(
                    det.box.height,
                    image.rows - y
                );

            if (width <= 0 ||
                height <= 0)
            {
                continue;
            }

            cv::Rect safeBox(
                x,
                y,
                width,
                height
            );

            // =================================================
            // Draw rectangle
            // =================================================

            cv::rectangle(
                image,
                safeBox,
                cv::Scalar(0, 255, 0),
                2
            );

            // =================================================
            // Class names
            // =================================================

            std::string className;

            if (det.class_id == 0)
            {
                className = "head";
            }
            else if (det.class_id == 1)
            {
                className = "helmet";
            }
            else if (det.class_id == 2)
            {
                className = "person";
            }
            else
            {
                className = "unknown";
            }

            // =================================================
            // Label
            // =================================================

            std::string label =
                className +
                " " +
                cv::format(
                    "%.2f",
                    det.confidence
                );

            // =================================================
            // Draw label
            // =================================================

            cv::putText(
                image,
                label,
                cv::Point(
                    x,
                    std::max(0, y - 10)
                ),
                cv::FONT_HERSHEY_SIMPLEX,
                0.5,
                cv::Scalar(0, 255, 0),
                2
            );
        }

        // ====================================================
        // Save processed image
        // ====================================================

        std::string outputPath =
            outputDir +
            "/processed_" +
            std::to_string(frameID) +
            ".jpg";

        bool saved =
            cv::imwrite(
                outputPath,
                image
            );

        if (saved)
        {
            std::cout
                << "Saved: "
                << outputPath
                << std::endl;
        }

        // ====================================================
        // Send processed image
        // ====================================================

        bool imageSent =
            sendImageToBaseNode(image);

        if (!imageSent)
        {
            std::cout
                << "Failed to send image."
                << std::endl;
        }

        // ====================================================
        // Send telemetry JSON
        // ====================================================

        bool telemetrySent =
            sendTelemetry(
                frameID,
                "camera_01",
                helmetFound,
                missingItems,
                bestConfidence,
                helmetFound ?
                    "safe" :
                    "alert"
            );

        if (!telemetrySent)
        {
            std::cout
                << "Failed to send telemetry."
                << std::endl;
        }

        // ====================================================
        // Delete received image
        // ====================================================

        try
        {
            fs::remove(
                receivedImagePath
            );
        }
        catch (...)
        {
            std::cout
                << "Failed to delete image."
                << std::endl;
        }

        // ====================================================
        // Increment frame ID
        // ====================================================

        frameID++;

        // ====================================================
        // Small delay
        // ====================================================

        std::this_thread::sleep_for(
            std::chrono::milliseconds(100)
        );
    }

    return 0;
}