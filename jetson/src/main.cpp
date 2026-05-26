#include "detector.hpp"

#include <iostream>
#include <vector>

#include <fstream>

#include <thread>
#include <chrono>

#include <opencv2/opencv.hpp>

#include <experimental/filesystem>

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

    std::string enginePath =
        argv[1];

    fs::path incomingDir =
        "images/incoming";

    fs::path outputDir =
        "images/output";

    fs::create_directories(incomingDir);
    fs::create_directories(outputDir);

    // ========================================================
    // Create detector
    // ========================================================

    YOLODetector detector(enginePath);

    // ========================================================
    // Continuous processing loop
    // ========================================================

    while (true)
    {
        std::vector<fs::path> files;

        // ====================================================
        // Find all images
        // ====================================================

        for (
            auto& entry :
            fs::recursive_directory_iterator(incomingDir)
        )
        {
            if (!fs::is_regular_file(entry.path()))
            {
                continue;
            }

            std::string ext =
                entry.path().extension().string();

            std::transform(
                ext.begin(),
                ext.end(),
                ext.begin(),
                ::tolower
            );

            if (
                ext == ".jpg"  ||
                ext == ".jpeg" ||
                ext == ".png"  ||
                ext == ".bmp"  ||
                ext == ".jfif"
            )
            {
                files.push_back(entry.path());
            }
        }

        // ====================================================
        // Process each image
        // ====================================================

        for (const auto& imagePath : files)
        {
            std::cout
                << "Processing: "
                << imagePath
                << std::endl;

            // =================================================
            // Load image
            // =================================================

            cv::Mat image =
                cv::imread(imagePath.string());

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

            auto detections =
                detector.infer(image);

            // =================================================
            // Draw detections
            // =================================================

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
                    cv::Point(det.box.x, det.box.y - 10),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.5,
                    cv::Scalar(0, 255, 0),
                    2
                );
            }

            bool helmetDetected = false;
            bool headDetected = false;

            float bestConfidence = 0.0f;

            for (const auto& det : detections)
            {
                if (det.class_id == 0)
                {
                    headDetected = true;
                }

                if (det.class_id == 1)
                {
                    helmetDetected = true;
                }

                if (det.confidence > bestConfidence)
                {
                    bestConfidence = det.confidence;
                }
            }

            bool ppeDetected =
                helmetDetected;

            std::string action =
                ppeDetected ? "none" : "alert";

            std::ofstream jsonFile(
                "images/output/latest_result.json"
            );

            fs::path outputPath =
            outputDir /
            imagePath.stem();

            outputPath += ".jpg";

            jsonFile
            << "{\n"
            << "  \"frame_id\": 1,\n"
            << "  \"node_id\": \"camera_01\",\n"
            << "  \"ppe_detected\": "
            << (ppeDetected ? "true" : "false")
            << ",\n"
            << "  \"confidence\": "
            << bestConfidence
            << ",\n"
            << "  \"missing_items\": "
            << (ppeDetected ? "[]" : "[\"helmet\"]")
            << ",\n"
            << "  \"action\": \""
            << action
            << "\",\n"
            << "  \"latest_image\": \""
            << outputPath.filename().string()
            << "\"\n"
            << "}\n";

            jsonFile.close();

            // =================================================
            // Save result
            // =================================================



            cv::imwrite(
                outputPath.string(),
                image
            );

            std::cout
                << "Saved: "
                << outputPath
                << std::endl;

            // =================================================
            // Delete original
            // =================================================

            fs::remove(imagePath);
        }

        // ====================================================
        // Avoid maxing CPU
        // ====================================================

        std::this_thread::sleep_for(
            std::chrono::milliseconds(500)
        );
    }

    return 0;
}