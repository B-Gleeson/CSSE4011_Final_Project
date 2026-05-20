// detector.hpp

#pragma once

#include <vector>
#include <string>
#include <memory>

#include <opencv2/opencv.hpp>

#include <NvInfer.h>
#include <cuda_runtime_api.h>

// ============================================================
// TensorRT Destroy Helper
// ============================================================

struct TRTDestroy
{
    template <typename T>
    void operator()(T* obj) const
    {
        if (obj)
        {
            obj->destroy();
        }
    }
};

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

    // ========================================================
    // TensorRT
    // ========================================================

    std::unique_ptr<
        nvinfer1::ICudaEngine,
        TRTDestroy
    > engine;

    std::unique_ptr<
        nvinfer1::IExecutionContext,
        TRTDestroy
    > context;

    // ========================================================
    // CUDA
    // ========================================================

    cudaStream_t stream;

    void* buffers[2];

    // ========================================================
    // Bindings
    // ========================================================

    int inputIndex;
    int outputIndex;

    // ========================================================
    // Input dimensions
    // ========================================================

    int inputWidth;
    int inputHeight;

    // ========================================================
    // Buffer sizes
    // ========================================================

    size_t inputSize;
    size_t outputSize;

public:

    YOLODetector(
        const std::string& enginePath
    );

    ~YOLODetector();

    std::vector<Detection>
    infer(cv::Mat& frame);
};