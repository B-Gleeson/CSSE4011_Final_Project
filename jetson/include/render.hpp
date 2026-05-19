// render.hpp

#pragma once

#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "detector.hpp"

// ============================================================
// Renderer
// ============================================================

class Renderer
{
public:

    void drawDetections(
        cv::Mat& frame,
        const std::vector<Detection>& detections
    );

    void drawFPS(
        cv::Mat& frame,
        float fps
    );

    void show(
        const std::string& windowName,
        cv::Mat& frame
    );
};