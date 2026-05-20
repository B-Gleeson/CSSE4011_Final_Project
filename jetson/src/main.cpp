// main.cpp

#include "detector.hpp"

#include <iostream>
#include <experimental/filesystem>
#include <thread>
#include <chrono>

namespace fs = std::experimental::filesystem;

int main(int argc, char** argv)
{
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

    std::string enginePath = argv[1];

    // ========================================================
    // Image directories
    // ========================================================

    std::string incomingDir = "receiver/images/incoming";

    std::string outputDir = "receiver/images/output";

    fs::create_directories(outputDir);

    // ========================================================
    // Create detector
    // ========================================================

    YOLODetector detector(enginePath);

    std::cout
        << "Watching folder: "
        << incomingDir
        << std::endl;

    // ========================================================
    // Main loop
    // ========================================================

    while (true)
    {
        for (const auto& entry :
             fs::directory_iterator(incomingDir))
        {
            std::string imagePath =
                entry.path().string();

            // ================================================
            // Only process jpg/jpeg
            // ================================================

            if (entry.path().extension() != ".jpg" &&
                entry.path().extension() != ".jpeg")
            {
                continue;
            }

            std::cout
                << "Processing: "
                << imagePath
                << std::endl;

            // ================================================
            // Load image
            // ================================================

            cv::Mat image =
                cv::imread(imagePath);

            if (image.empty())
            {
                std::cout
                    << "Failed to load image."
                    << std::endl;

                continue;
            }

            // ================================================
            // Run inference
            // ================================================

            auto detections =
                detector.infer(image);

            // ================================================
            // Draw detections
            // ================================================

            for (const auto& det : detections)
            {
                cv::rectangle(
                    image,
                    det.box,
                    cv::Scalar(0, 255, 0),
                    2
                );

                std::string className;

                if (det.class_id == 0)
                {
                    className = "head";
                }
                else if (det.class_id == 1)
                {
                    className = "helmet";
                }
                else
                {
                    className = "unknown";
                }

                std::string label =
                    className +
                    " " +
                    std::to_string(det.confidence);

                cv::putText(
                    image,
                    label,
                    cv::Point(
                        det.box.x,
                        det.box.y - 10
                    ),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.5,
                    cv::Scalar(0, 255, 0),
                    2
                );
            }

            // ================================================
            // Save output image
            // ================================================

            std::string filename =
                entry.path().filename().string();

            std::string outputPath =
                outputDir + "/" + filename;

            cv::imwrite(outputPath, image);

            std::cout
                << "Saved: "
                << outputPath
                << std::endl;

            // ================================================
            // Delete processed image
            // ================================================

            fs::remove(imagePath);
        }

        // ====================================================
        // Small delay
        // ====================================================

        std::this_thread::sleep_for(
            std::chrono::milliseconds(500)
        );
    }

    return 0;
}