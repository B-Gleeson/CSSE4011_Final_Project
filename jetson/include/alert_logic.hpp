// alert_logic.hpp

#pragma once

#include <vector>

#include <opencv2/opencv.hpp>

#include "detector.hpp"

// ============================================================
// Alert Logic
// ============================================================

class AlertLogic
{
public:

    bool hasViolation(
        const std::vector<Detection>& detections
    );

    void triggerAlert();

    void drawAlertOverlay(cv::Mat& frame);
};