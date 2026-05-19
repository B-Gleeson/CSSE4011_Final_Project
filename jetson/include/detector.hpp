// detector.hpp

#pragma once

#include <vector>
#include <string>
#include <memory>

#include <opencv2/opencv.hpp>

#include <NvInfer.h>
#include <cuda_runtime_api.h>

// ============================================================
// Detection Structure
// ============================================================

struct Detection
{
    cv::Rect box;

    float confidence;

    int class_id;
};

// ============================================================
// YOLO Detector
// ============================================================

class YOLODetector
{
private:

    std::unique_ptr<nvinfer1::ICudaEngine> engine;

    std::unique_ptr<nvinfer1::IExecutionContext> context;

    cudaStream_t stream;

    void* buffers[2];

    int inputIndex;
    int outputIndex;

    int inputWidth;
    int inputHeight;

    size_t inputSize;
    size_t outputSize;

public:

    YOLODetector(const std::string& enginePath);

    ~YOLODetector();

    std::vector<Detection> infer(cv::Mat& frame);
};