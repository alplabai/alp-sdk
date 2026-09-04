/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only stand-in for upstream ONNX Runtime's onnxruntime_c_api.h.
 *
 * NOT a copy of Microsoft's header.  This backend (src/yocto/inference_ort.cpp)
 * is written against ORT's real C API, but that API is staged onto the Yocto
 * sysroot at build time (meta-alp-sdk's own onnxruntime recipe) and is not
 * installed on this dev host or in CI (issue #1747: no PR gate builds this
 * backend for that reason).  This file declares -- from scratch, matching
 * only the function NAMES/signatures/enum values inference_ort.cpp's own
 * "Surface used here" file-header comment documents calling -- a fake
 * OrtApi/OrtApiBase vtable a test can populate with its own function
 * pointers.  Struct member ORDER does not need to match the real ABI: this
 * is never linked against a real libonnxruntime.so, only against a test's
 * own fake implementation, so name-based member access is all that matters.
 */
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

typedef struct OrtEnv                    OrtEnv;
typedef struct OrtSession                OrtSession;
typedef struct OrtSessionOptions         OrtSessionOptions;
typedef struct OrtMemoryInfo             OrtMemoryInfo;
typedef struct OrtStatus                 OrtStatus;
typedef struct OrtTypeInfo               OrtTypeInfo;
typedef struct OrtTensorTypeAndShapeInfo OrtTensorTypeAndShapeInfo;
typedef struct OrtValue                  OrtValue;
typedef struct OrtRunOptions             OrtRunOptions;

typedef enum OrtLoggingLevel {
	ORT_LOGGING_LEVEL_VERBOSE = 0,
	ORT_LOGGING_LEVEL_INFO,
	ORT_LOGGING_LEVEL_WARNING,
	ORT_LOGGING_LEVEL_ERROR,
	ORT_LOGGING_LEVEL_FATAL,
} OrtLoggingLevel;

typedef enum OrtErrorCode {
	ORT_OK = 0,
	ORT_FAIL,
	ORT_INVALID_ARGUMENT,
	ORT_NO_SUCHFILE,
	ORT_NO_MODEL,
	ORT_ENGINE_ERROR,
	ORT_RUNTIME_EXCEPTION,
	ORT_INVALID_PROTOBUF,
	ORT_MODEL_LOADED,
	ORT_NOT_IMPLEMENTED,
	ORT_INVALID_GRAPH,
	ORT_EP_FAIL,
} OrtErrorCode;

typedef enum ONNXTensorElementDataType {
	ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED = 0,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX64,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX128,
	ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16,
} ONNXTensorElementDataType;

typedef enum OrtAllocatorType {
	OrtInvalidAllocator = -1,
	OrtDeviceAllocator  = 0,
	OrtArenaAllocator   = 1,
} OrtAllocatorType;

typedef enum OrtMemType {
	OrtMemTypeCPUInput  = -2,
	OrtMemTypeCPUOutput = -1,
	OrtMemTypeDefault   = 0,
} OrtMemType;

typedef struct OrtAllocator {
	uint32_t version;
	void *(*Alloc)(struct OrtAllocator *this_, size_t size);
	void (*Free)(struct OrtAllocator *this_, void *p);
	const OrtMemoryInfo *(*Info)(const struct OrtAllocator *this_);
} OrtAllocator;

#define ORT_API_VERSION 18u

typedef struct OrtApi {
	OrtStatus *(*CreateEnv)(OrtLoggingLevel log_severity_level, const char *logid, OrtEnv **out);
	OrtStatus *(*CreateSessionOptions)(OrtSessionOptions **options);
	void (*ReleaseSessionOptions)(OrtSessionOptions *input);
	OrtStatus *(*CreateSessionFromArray)(const OrtEnv            *env,
	                                     const void              *model_data,
	                                     size_t                   model_data_length,
	                                     const OrtSessionOptions *options,
	                                     OrtSession             **out);
	OrtStatus *(*CreateCpuMemoryInfo)(OrtAllocatorType type,
	                                  OrtMemType       mem_type,
	                                  OrtMemoryInfo  **out);
	OrtStatus *(*GetAllocatorWithDefaultOptions)(OrtAllocator **out);
	OrtStatus *(*SessionGetInputCount)(const OrtSession *session, size_t *out);
	OrtStatus *(*SessionGetOutputCount)(const OrtSession *session, size_t *out);
	OrtStatus *(*SessionGetInputTypeInfo)(const OrtSession *session,
	                                      size_t            index,
	                                      OrtTypeInfo     **type_info);
	OrtStatus *(*SessionGetOutputTypeInfo)(const OrtSession *session,
	                                       size_t            index,
	                                       OrtTypeInfo     **type_info);
	OrtStatus *(*CastTypeInfoToTensorInfo)(const OrtTypeInfo                *type_info,
	                                       const OrtTensorTypeAndShapeInfo **out);
	OrtStatus *(*GetTensorElementType)(const OrtTensorTypeAndShapeInfo *info,
	                                   ONNXTensorElementDataType       *out);
	OrtStatus *(*GetDimensionsCount)(const OrtTensorTypeAndShapeInfo *info, size_t *out);
	OrtStatus *(*GetDimensions)(const OrtTensorTypeAndShapeInfo *info,
	                            int64_t                         *dim_values,
	                            size_t                           dim_values_length);
	void (*ReleaseTypeInfo)(OrtTypeInfo *input);
	OrtStatus *(*SessionGetInputName)(const OrtSession *session,
	                                  size_t            index,
	                                  OrtAllocator     *allocator,
	                                  char            **value);
	OrtStatus *(*SessionGetOutputName)(const OrtSession *session,
	                                   size_t            index,
	                                   OrtAllocator     *allocator,
	                                   char            **value);
	OrtStatus *(*CreateTensorWithDataAsOrtValue)(const OrtMemoryInfo      *info,
	                                             void                     *p_data,
	                                             size_t                    p_data_len,
	                                             const int64_t            *shape,
	                                             size_t                    shape_len,
	                                             ONNXTensorElementDataType type,
	                                             OrtValue                **out);
	void (*ReleaseValue)(OrtValue *input);
	void (*ReleaseMemoryInfo)(OrtMemoryInfo *input);
	void (*ReleaseSession)(OrtSession *input);
	void (*ReleaseEnv)(OrtEnv *input);
	OrtErrorCode (*GetErrorCode)(const OrtStatus *status);
	const char *(*GetErrorMessage)(const OrtStatus *status);
	void (*ReleaseStatus)(OrtStatus *input);
	OrtStatus *(*Run)(OrtSession            *session,
	                  const OrtRunOptions   *run_options,
	                  const char *const     *input_names,
	                  const OrtValue *const *inputs,
	                  size_t                 input_len,
	                  const char *const     *output_names,
	                  size_t                 output_names_len,
	                  OrtValue             **outputs);
} OrtApi;

typedef struct OrtApiBase {
	const OrtApi *(*GetApi)(uint32_t version);
	const char *(*GetVersionString)(void);
} OrtApiBase;

const OrtApiBase *OrtGetApiBase(void);

} /* extern "C" */
