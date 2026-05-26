#include "detector.hpp"

#include <iostream>
#include <vector>
#include <fstream>

#include <thread>
#include <chrono>

#include <opencv2/opencv.hpp>

#include <experimental/filesystem>

// Filesystem namespace alias
namespace fs = std::experimental::filesystem;

// ============================================================
// Main
//
// Continuously scans the incoming image directory for
// newly uploaded images from the ESP32-CAM.
//
// Each image is:
//  1. Loaded into memory
//  2. Processed using the YOLO TensorRT detector
//  3. Annotated with bounding boxes + labels
//  4. Saved into the output directory
//  5. Deleted from the incoming directory
//
// The program also generates a JSON telemetry file
// containing the latest PPE detection result for
// communication with the M5Core base station.
// ============================================================

int main(int argc, char** argv)
{
        // Validate command line arguments
    if (argc < 2)
    {
        std::cout
            << "Usage: ./ppe_inference engine"
            << std::endl;

        return -1;
    }

    // TensorRT engine file path
    std::string enginePath =
        argv[1];

    // Image directories
    fs::path incomingDir =
        "images/incoming";

    fs::path outputDir =
        "images/output";

    // Create directories if they do not exist
    fs::create_directories(incomingDir);
    fs::create_directories(outputDir);

    // Create YOLO detector
    YOLODetector detector(enginePath);

    int frameId = 0;

    // Continuous processing loop
    while (true)
    {
        // ====================================================
        // List of images to process
        //
        // We store files first before processing so that
        // deleting files later does not invalidate the
        // recursive directory iterator.
        // ====================================================
        std::vector<fs::path> files;

        // Recursively search incoming directory
        for (
            auto& entry :
            fs::recursive_directory_iterator(incomingDir)
        )
        {
            // Skip anything that is not a normal file
            if (!fs::is_regular_file(entry.path()))
            {
                continue;
            }

            // Get file extension
            std::string ext =
                entry.path().extension().string();

            // =================================================
            // Convert extension to lowercase so that
            // JPG, JPG, Jpg etc all work correctly
            // =================================================
            std::transform(
                ext.begin(),
                ext.end(),
                ext.begin(),
                ::tolower
            );

            // Accept supported image types only
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

        // Process all discovered images
        for (const auto& imagePath : files)
        {
            std::cout
                << "Processing: "
                << imagePath
                << std::endl;

            // Load image using OpenCV
            cv::Mat image =
                cv::imread(imagePath.string());

            // Validate image loaded correctly
            if (image.empty())
            {
                std::cout
                    << "Failed to load image."
                    << std::endl;

                continue;
            }

            frameId++;

            // =================================================
            // Run TensorRT YOLO inference
            //
            // Returns a vector of detections containing:
            //  - Bounding box
            //  - Class ID
            //  - Confidence score
            // =================================================
            auto detections =
                detector.infer(image);

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

            // =================================================
            // AI telemetry generation
            //
            // Determine:
            //  - whether a helmet was detected
            //  - highest confidence detection
            //  - whether an alert should be raised
            // =================================================

            bool helmetDetected = false;
            bool headDetected = false;

            float bestConfidence = 0.0f;

            for (const auto& det : detections)
            {
                // Head detected
                if (det.class_id == 0)
                {
                    headDetected = true;
                }

                // Helmet detected
                if (det.class_id == 1)
                {
                    helmetDetected = true;
                }

                // Store highest confidence value
                if (det.confidence > bestConfidence)
                {
                    bestConfidence = det.confidence;
                }
            }

            bool ppeDetected =
                helmetDetected;

            // Alert action state
            std::string action =
                ppeDetected ? "none" : "alert";


            // =================================================
            // Write latest telemetry JSON
            //
            // This file is read by transmitter.py and served
            // to the M5Core base station.
            // =================================================

            std::ofstream jsonFile(
                "images/output/latest_result.json"
            );

            fs::path outputPath =
            outputDir /
            imagePath.stem();

            outputPath += ".jpg";

            jsonFile
            << "{\n"
            << "  \"frame_id\": " << frameId << ",\n"
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

            // Save annotated image
            cv::imwrite(
                outputPath.string(),
                image
            );

            std::cout
                << "Saved: "
                << outputPath
                << std::endl;

            // =================================================
            // Delete original input image
            //
            // Prevents duplicate processing.
            // =================================================

            fs::remove(imagePath);
        }

        // Sleep briefly to avoid maxing CPU usage
        std::this_thread::sleep_for(
            std::chrono::milliseconds(500)
        );
    }

    return 0;
}