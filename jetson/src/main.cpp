// main.cpp

#include "detector.hpp"

#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <set>

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
        "images/incoming";

    std::string outputDir =
        "images/output";

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
    // Track processed files
    // ========================================================

    std::set<std::string>
        processedFiles;

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
        // Iterate incoming folder
        // ====================================================

        for (const auto& entry :
             fs::directory_iterator(
                 incomingDir))
        {
            // =================================================
            // Path
            // =================================================

            std::string imagePath =
                entry.path().string();

            // =================================================
            // Only jpg
            // =================================================

            if (entry.path().extension()
                != ".jpg")
            {
                continue;
            }

            // =================================================
            // Skip already processed
            // =================================================

            if (processedFiles.count(
                    imagePath))
            {
                continue;
            }

            std::cout
                << "Processing: "
                << imagePath
                << std::endl;

            // =================================================
            // Load image
            // =================================================

            cv::Mat image =
                cv::imread(imagePath);

            if (image.empty())
            {
                std::cout
                    << "Failed to load image."
                    << std::endl;

                continue;
            }

            // =================================================
            // Run inference
            // =================================================

            std::vector<Detection>
                detections =
                    detector.infer(image);

            // =================================================
            // Draw detections
            // =================================================

            for (const auto& det :
                 detections)
            {
                // =============================================
                // Clamp box
                // =============================================

                int x =
                    std::max(
                        0,
                        det.box.x
                    );

                int y =
                    std::max(
                        0,
                        det.box.y
                    );

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

                // =============================================
                // Draw rectangle
                // =============================================

                cv::rectangle(
                    image,
                    safeBox,
                    cv::Scalar(
                        0,
                        255,
                        0
                    ),
                    2
                );

                // =============================================
                // Class names
                // =============================================

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

                // =============================================
                // Label
                // =============================================

                std::string label =
                    className +
                    " " +
                    cv::format(
                        "%.2f",
                        det.confidence
                    );

                // =============================================
                // Draw label
                // =============================================

                cv::putText(
                    image,
                    label,
                    cv::Point(
                        x,
                        std::max(
                            0,
                            y - 10
                        )
                    ),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.5,
                    cv::Scalar(
                        0,
                        255,
                        0
                    ),
                    2
                );
            }

            // =================================================
            // Output filename
            // =================================================

            std::string filename =
                entry.path().filename()
                .string();

            std::string outputPath =
                outputDir +
                "/processed_" +
                filename;

            // =================================================
            // Save output image
            // =================================================

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

            // =================================================
            // Mark processed
            // =================================================

            processedFiles.insert(
                imagePath
            );

            frameID++;
        }

        // ====================================================
        // Small delay
        // ====================================================

        std::this_thread::sleep_for(
            std::chrono::milliseconds(
                100
            )
        );
    }

    return 0;
}