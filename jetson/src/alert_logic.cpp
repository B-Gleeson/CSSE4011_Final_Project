// alert_logic.cpp

#include "alert_logic.hpp"

#include <iostream>

// ============================================================
// Check if a no-hardhat violation exists
// ============================================================

bool AlertLogic::hasViolation(
    const std::vector<Detection>& detections
)
{
    for (const auto& det : detections)
    {
        // ============================================
        // Class IDs
        //
        // 0 = hardhat
        // 1 = no_hardhat
        //
        // Adjust depending on your dataset.yaml
        // ============================================

        if (det.class_id == 1)
        {
            return true;
        }
    }

    return false;
}

// ============================================================
// Print warning
// ============================================================

void AlertLogic::triggerAlert()
{
    std::cout << std::endl;
    std::cout << "================================="
              << std::endl;

    std::cout << "WARNING: NO HARDHAT DETECTED"
              << std::endl;

    std::cout << "================================="
              << std::endl;
}

// ============================================================
// Draw warning overlay
// ============================================================

void AlertLogic::drawAlertOverlay(cv::Mat& frame)
{
    cv::rectangle(
        frame,
        cv::Point(0, 0),
        cv::Point(frame.cols, 80),
        cv::Scalar(0, 0, 255),
        cv::FILLED
    );

    cv::putText(
        frame,
        "WARNING: NO HARDHAT DETECTED",
        cv::Point(20, 50),
        cv::FONT_HERSHEY_SIMPLEX,
        1.0,
        cv::Scalar(255,255,255),
        2
    );
}