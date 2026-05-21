// telemetry.hpp

#pragma once

#include <vector>
#include <string>

bool sendTelemetry(
    int frameID,
    const std::string& nodeID,
    bool ppeDetected,
    const std::vector<std::string>& missingItems,
    float confidence,
    const std::string& action
);