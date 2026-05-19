// render.cpp

#include "render.hpp"

#include <opencv2/opencv.hpp>

// ============================================================
// Draw detections
// ============================================================

void Renderer::drawDetections(
    cv::Mat& frame,
    const std::vector<Detection>& detections
)
{
    for (const auto& det : detections)
    {
        cv::Scalar color;

        // ============================================
        // Class colors
        //
        // 0 = hardhat
        // 1 = no_hardhat
        // ============================================

        if (det.class_id == 0)
        {
            // Green = safe
            color = cv::Scalar(0, 255, 0);
        }
        else
        {
            // Red = violation
            color = cv::Scalar(0, 0, 255);
        }

        // ============================================
        // Draw bounding box
        // ============================================

        cv::rectangle(
            frame,
            det.box,
            color,
            2
        );

        // ============================================
        // Build label
        // ============================================

        std::string className;

        if (det.class_id == 0)
        {
            className = "hardhat";
        }
        else
        {
            className = "no_hardhat";
        }

        std::string label =
            className +
            " " +
            std::to_string(det.confidence);

        // ============================================
        // Draw label text
        // ============================================

        cv::putText(
            frame,
            label,
            cv::Point(
                det.box.x,
                det.box.y - 10
            ),
            cv::FONT_HERSHEY_SIMPLEX,
            0.5,
            color,
            2
        );
    }
}

// ============================================================
// Draw FPS
// ============================================================

void Renderer::drawFPS(
    cv::Mat& frame,
    float fps
)
{
    cv::putText(
        frame,
        "FPS: " + std::to_string(fps),
        cv::Point(20, 40),
        cv::FONT_HERSHEY_SIMPLEX,
        1.0,
        cv::Scalar(255,255,255),
        2
    );
}

// ============================================================
// Show frame
// ============================================================

void Renderer::show(
    const std::string& windowName,
    cv::Mat& frame
)
{
    cv::imshow(windowName, frame);
}