// detector.cpp

#include "detector.hpp"

#include <iostream>
#include <fstream>
#include <algorithm>

#include <NvInfer.h>
#include <cuda_runtime_api.h>

// ============================================================
// TensorRT Logger
// ============================================================

class Logger : public nvinfer1::ILogger
{
public:
    void log(Severity severity, const char* msg) noexcept override
    {
        if (severity <= Severity::kWARNING)
        {
            std::cout << msg << std::endl;
        }
    }
};

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
// Private Globals
// ============================================================

static Logger logger;

// ============================================================
// Constructor
// ============================================================

YOLODetector::YOLODetector(const std::string& enginePath)
{
    // ============================================
    // Load TensorRT engine file
    // ============================================

    std::ifstream file(enginePath, std::ios::binary);

    if (!file.good())
    {
        throw std::runtime_error(
            "Failed to open engine file."
        );
    }

    file.seekg(0, std::ios::end);
    size_t modelSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> engineData(modelSize);

    file.read(engineData.data(), modelSize);

    // ============================================
    // Create TensorRT runtime
    // ============================================

    std::unique_ptr<nvinfer1::IRuntime, TRTDestroy>
        runtime(
            nvinfer1::createInferRuntime(logger)
        );

    engine.reset(
        runtime->deserializeCudaEngine(
            engineData.data(),
            modelSize
        )
    );

    if (!engine)
    {
        throw std::runtime_error(
            "Failed to deserialize TensorRT engine."
        );
    }

    // ============================================
    // Create execution context
    // ============================================

    context.reset(
        engine->createExecutionContext()
    );

    if (!context)
    {
        throw std::runtime_error(
            "Failed to create TensorRT context."
        );
    }

    // ============================================
    // Create CUDA stream
    // ============================================

    cudaStreamCreate(&stream);

    // ============================================
    // Locate bindings
    // ============================================

    inputIndex  = engine->getBindingIndex("images");
    outputIndex = engine->getBindingIndex("output0");

    auto inputDims =
        engine->getBindingDimensions(inputIndex);

    auto outputDims =
        engine->getBindingDimensions(outputIndex);

    inputHeight = inputDims.d[2];
    inputWidth  = inputDims.d[3];

    // ============================================
    // Calculate buffer sizes
    // ============================================

    inputSize = 1;

    for (int i = 0; i < inputDims.nbDims; i++)
    {
        inputSize *= inputDims.d[i];
    }

    inputSize *= sizeof(float);

    outputSize = 1;

    for (int i = 0; i < outputDims.nbDims; i++)
    {
        outputSize *= outputDims.d[i];
    }

    outputSize *= sizeof(float);

    // ============================================
    // Allocate GPU memory
    // ============================================

    cudaMalloc(&buffers[inputIndex], inputSize);
    cudaMalloc(&buffers[outputIndex], outputSize);

    std::cout << "YOLO detector initialized."
              << std::endl;
}

// ============================================================
// Destructor
// ============================================================

YOLODetector::~YOLODetector()
{
    cudaFree(buffers[inputIndex]);
    cudaFree(buffers[outputIndex]);

    cudaStreamDestroy(stream);
}

// ============================================================
// Inference
// ============================================================

std::vector<Detection>
YOLODetector::infer(cv::Mat& frame)
{
    // ============================================
    // Resize image
    // ============================================

    cv::Mat resized;

    cv::resize(
        frame,
        resized,
        cv::Size(inputWidth, inputHeight)
    );

    // ============================================
    // Normalize
    // ============================================

    resized.convertTo(
        resized,
        CV_32F,
        1.0 / 255.0
    );

    // ============================================
    // HWC -> CHW
    // ============================================

    std::vector<float>
        inputTensor(inputSize / sizeof(float));

    int channels = 3;

    for (int c = 0; c < channels; c++)
    {
        for (int y = 0; y < inputHeight; y++)
        {
            for (int x = 0; x < inputWidth; x++)
            {
                inputTensor[
                    c * inputHeight * inputWidth +
                    y * inputWidth +
                    x
                ] = resized.at<cv::Vec3f>(y, x)[c];
            }
        }
    }

    // ============================================
    // Copy input to GPU
    // ============================================

    cudaMemcpyAsync(
        buffers[inputIndex],
        inputTensor.data(),
        inputSize,
        cudaMemcpyHostToDevice,
        stream
    );

    // ============================================
    // Run inference
    // ============================================

    context->enqueueV2(
        buffers,
        stream,
        nullptr
    );

    // ============================================
    // Copy output from GPU
    // ============================================

    std::vector<float>
        output(outputSize / sizeof(float));

    cudaMemcpyAsync(
        output.data(),
        buffers[outputIndex],
        outputSize,
        cudaMemcpyDeviceToHost,
        stream
    );

    cudaStreamSynchronize(stream);

    // ============================================
    // YOLO Postprocessing
    // ============================================

    std::vector<Detection> detections;

    const int numPredictions = 8400;
    const int dimensions = 6;

    for (int i = 0; i < numPredictions; i++)
    {
        float confidence =
            output[i * dimensions + 4];

        if (confidence < 0.5f)
        {
            continue;
        }

        int class_id =
            (int)output[i * dimensions + 5];

        float cx =
            output[i * dimensions + 0];

        float cy =
            output[i * dimensions + 1];

        float w =
            output[i * dimensions + 2];

        float h =
            output[i * dimensions + 3];

        int left =
            static_cast<int>(
                (cx - w / 2.0f) *
                frame.cols / inputWidth
            );

        int top =
            static_cast<int>(
                (cy - h / 2.0f) *
                frame.rows / inputHeight
            );

        int width =
            static_cast<int>(
                w * frame.cols / inputWidth
            );

        int height =
            static_cast<int>(
                h * frame.rows / inputHeight
            );

        Detection det;

        det.box =
            cv::Rect(
                left,
                top,
                width,
                height
            );

        det.confidence = confidence;
        det.class_id = class_id;

        detections.push_back(det);
    }

    return detections;
}