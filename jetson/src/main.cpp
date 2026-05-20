// main.cpp

#include "detector.hpp"

#include <iostream>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cout
            << "Usage: ./ppe_inference engine image"
            << std::endl;

        return -1;
    }

    std::string enginePath = argv[1];
    std::string imagePath  = argv[2];

    // ========================================================
    // Load image
    // ========================================================

    cv::Mat image = cv::imread(imagePath);

    if (image.empty())
    {
        std::cout
            << "Failed to load image."
            << std::endl;

        return -1;
    }

    // ========================================================
    // Create detector
    // ========================================================

    YOLODetector detector(enginePath);

    // ========================================================
    // Run inference
    // ========================================================

    auto detections =
        detector.infer(image);

    // ========================================================
    // Draw detections
    // ========================================================

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

    // ========================================================
    // Show result
    // ========================================================

    cv::imwrite("output.jpg", image);

    std::cout
        << "Saved output.jpg"
        << std::endl;

    return 0;
}