// detector.cpp

#include "detector.hpp"

#include <iostream>
#include <fstream>
#include <algorithm>

#include <NvInfer.h>
#include <cuda_runtime_api.h>

#include <opencv2/opencv.hpp>
#include <opencv2/dnn/dnn.hpp>

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

static Logger logger;

// ============================================================
// Constructor
// ============================================================

YOLODetector::YOLODetector(const std::string& enginePath)
{
    // ========================================================
    // Load engine file
    // ========================================================

    std::ifstream file(enginePath, std::ios::binary);

    if (!file.good())
    {
        throw std::runtime_error(
            "Failed to open TensorRT engine."
        );
    }

    file.seekg(0, std::ios::end);

    size_t modelSize = file.tellg();

    file.seekg(0, std::ios::beg);

    std::vector<char> engineData(modelSize);

    file.read(engineData.data(), modelSize);

    file.close();

    // ========================================================
    // Create runtime
    // ========================================================

    std::unique_ptr<nvinfer1::IRuntime, TRTDestroy>
        runtime(
            nvinfer1::createInferRuntime(logger)
        );

    if (!runtime)
    {
        throw std::runtime_error(
            "Failed to create TensorRT runtime."
        );
    }

    // ========================================================
    // Deserialize engine
    // ========================================================

    engine.reset(
        runtime->deserializeCudaEngine(
            engineData.data(),
            modelSize
        )
    );

    if (!engine)
    {
        throw std::runtime_error(
            "Failed to deserialize engine."
        );
    }

    // ========================================================
    // Create execution context
    // ========================================================

    context.reset(
        engine->createExecutionContext()
    );

    if (!context)
    {
        throw std::runtime_error(
            "Failed to create execution context."
        );
    }

    // ========================================================
    // Print bindings
    // ========================================================

    std::cout << "Bindings:" << std::endl;

    for (int i = 0; i < engine->getNbBindings(); i++)
    {
        std::cout
            << i
            << " -> "
            << engine->getBindingName(i)
            << std::endl;
    }

    // ========================================================
    // Binding indices
    // ========================================================

    inputIndex  = engine->getBindingIndex("images");
    outputIndex = engine->getBindingIndex("output0");

    if (inputIndex < 0 || outputIndex < 0)
    {
        throw std::runtime_error(
            "Failed to find bindings."
        );
    }

    // ========================================================
    // Input dimensions
    // ========================================================

    auto inputDims =
        engine->getBindingDimensions(inputIndex);

    auto outputDims =
        engine->getBindingDimensions(outputIndex);

    inputHeight = inputDims.d[2];
    inputWidth  = inputDims.d[3];

    // ========================================================
    // Calculate sizes
    // ========================================================

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

    // ========================================================
    // Allocate GPU memory
    // ========================================================

    cudaError_t status;

    status = cudaMalloc(
        &buffers[inputIndex],
        inputSize
    );

    if (status != cudaSuccess)
    {
        throw std::runtime_error(
            "Failed to allocate input buffer."
        );
    }

    status = cudaMalloc(
        &buffers[outputIndex],
        outputSize
    );

    if (status != cudaSuccess)
    {
        throw std::runtime_error(
            "Failed to allocate output buffer."
        );
    }

    // ========================================================
    // Create CUDA stream
    // ========================================================

    cudaStreamCreate(&stream);

    std::cout
        << "YOLO detector initialized."
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
    // ========================================================
    // Preprocessing
    // ========================================================

    cv::Mat resized;

    cv::resize(
        frame,
        resized,
        cv::Size(inputWidth, inputHeight)
    );

    // BGR -> RGB

    cv::cvtColor(
        resized,
        resized,
        cv::COLOR_BGR2RGB
    );

    // Normalize

    resized.convertTo(
        resized,
        CV_32F,
        1.0 / 255.0
    );

    // ========================================================
    // HWC -> CHW
    // ========================================================

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

    // ========================================================
    // Copy input to GPU
    // ========================================================

    cudaMemcpyAsync(
        buffers[inputIndex],
        inputTensor.data(),
        inputSize,
        cudaMemcpyHostToDevice,
        stream
    );

    // ========================================================
    // Run inference
    // ========================================================

    bool success =
        context->enqueueV2(
            buffers,
            stream,
            nullptr
        );

    if (!success)
    {
        throw std::runtime_error(
            "TensorRT inference failed."
        );
    }

    // ========================================================
    // Copy output back
    // ========================================================

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

        // ========================================================
    // YOLOv8 Postprocessing
    // ========================================================

    std::vector<Detection> detections;

    std::vector<cv::Rect> boxes;
    std::vector<float> confidences;
    std::vector<int> class_ids;

    const int numPredictions = 8400;

    const int numClasses = 2;

    const float confThreshold = 0.5f;

    const float nmsThreshold = 0.4f;

    // Output shape:
    // [1, 4 + numClasses, 8400]

    for (int i = 0; i < numPredictions; i++)
    {
        // ================================================
        // Box coordinates
        // ================================================

        float cx = output[0 * numPredictions + i];

        float cy = output[1 * numPredictions + i];

        float w  = output[2 * numPredictions + i];

        float h  = output[3 * numPredictions + i];

        // ================================================
        // Find best class
        // ================================================

        int class_id = -1;

        float bestScore = 0.0f;

        for (int c = 0; c < numClasses; c++)
        {
            float score =
                output[(4 + c) * numPredictions + i];

            if (score > bestScore)
            {
                bestScore = score;

                class_id = c;
            }
        }

        // ================================================
        // Confidence threshold
        // ================================================

        if (bestScore < confThreshold)
        {
            continue;
        }

        // ================================================
        // Convert coordinates
        // ================================================

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

        left = std::max(0, left);
        top = std::max(0, top);

        width =
            std::min(width, frame.cols - left);

        height =
            std::min(height, frame.rows - top);

        boxes.push_back(
            cv::Rect(left, top, width, height)
        );

        confidences.push_back(bestScore);

        class_ids.push_back(class_id);
    }

    // ========================================================
    // Non-Max Suppression
    // ========================================================

    std::vector<int> indices;

    cv::dnn::NMSBoxes(
        boxes,
        confidences,
        confThreshold,
        nmsThreshold,
        indices
    );

    // ========================================================
    // Final detections
    // ========================================================

    for (int idx : indices)
    {
        Detection det;

        det.box = boxes[idx];

        det.confidence = confidences[idx];

        det.class_id = class_ids[idx];

        detections.push_back(det);
    }

    return detections;
}