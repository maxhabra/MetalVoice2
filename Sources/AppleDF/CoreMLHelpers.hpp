#pragma once
#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#include <cmath>
#include <cstdio>
#include <cstring>

static void errorText(char *error, size_t capacity, NSString *text) {
    if (error && capacity) std::snprintf(error, capacity, "%s", text.UTF8String ?: "Core ML failed");
}

static uint32_t devicesInBlock(MLComputePlan *plan, MLModelStructureProgramBlock *block) {
    uint32_t devices = 0;
    for (MLModelStructureProgramOperation *op in block.operations) {
        id<MLComputeDeviceProtocol> device = [plan computeDeviceUsageForMLProgramOperation:op].preferredComputeDevice;
        if ([device isKindOfClass:MLCPUComputeDevice.class]) devices |= 1;
        if ([device isKindOfClass:MLGPUComputeDevice.class]) devices |= 2;
        if ([device isKindOfClass:MLNeuralEngineComputeDevice.class]) devices |= 4;
        for (MLModelStructureProgramBlock *nested in op.blocks) devices |= devicesInBlock(plan, nested);
    }
    return devices;
}

static uint32_t modelDevices(NSURL *url, MLModelConfiguration *config) {
    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    __block uint32_t devices = 0;
    [MLComputePlan loadContentsOfURL:url configuration:config completionHandler:^(MLComputePlan *plan, NSError *) {
        for (MLModelStructureProgramFunction *function in plan.modelStructure.program.functions.allValues)
            devices |= devicesInBlock(plan, function.block);
        dispatch_semaphore_signal(ready);
    }];
    return dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) == 0 ? devices : 0;
}

static bool copyOutput(MLMultiArray *array, float *output, size_t count) {
    if (!array || size_t(array.count) != count || !array.dataPointer) return false;
    if (array.dataType != MLMultiArrayDataTypeFloat32 && array.dataType != MLMultiArrayDataTypeFloat16) return false;
    const NSUInteger rank = array.shape.count;
    if (rank > 5) return false;
    size_t shape[5]{}, strides[5]{};
    bool contiguous = true;
    size_t expected = 1;
    for (NSUInteger i = rank; i-- > 0;) {
        shape[i] = array.shape[i].unsignedIntegerValue;
        strides[i] = array.strides[i].unsignedIntegerValue;
        if (shape[i] != 1 && strides[i] != expected) contiguous = false;
        expected *= shape[i];
    }
    if (contiguous && array.dataType == MLMultiArrayDataTypeFloat32) {
        std::memmove(output, array.dataPointer, count * sizeof(float));
    } else {
        for (size_t index = 0; index < count; ++index) {
            size_t remaining = index, offset = 0;
            for (NSUInteger i = rank; i-- > 0;) { offset += (remaining % shape[i]) * strides[i]; remaining /= shape[i]; }
            output[index] = array.dataType == MLMultiArrayDataTypeFloat32
                ? static_cast<const float *>(array.dataPointer)[offset]
                : float(static_cast<const _Float16 *>(array.dataPointer)[offset]);
        }
    }
    for (size_t i = 0; i < count; ++i) if (!std::isfinite(output[i])) return false;
    return true;
}

