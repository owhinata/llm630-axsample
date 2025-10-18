// Error codes and helpers for axsys C++ API
#pragma once

#include <string>

namespace axsys {

enum class ErrorCode {
  kSuccess = 0,

  // General errors (1-99)
  kInvalidArgument = 1,
  kOutOfRange = 2,
  kNotInitialized = 3,
  kAlreadyInitialized = 4,

  // Memory errors (100-199)
  kAllocationFailed = 100,
  kMemoryTooLarge = 101,
  kNoAllocation = 102,
  kNotOwned = 103,
  kReferencesRemain = 104,
  kMemFreeFailed = 105,

  // View/Mapping errors (200-299)
  kMapFailed = 200,
  kUnmapFailed = 201,
  kFlushFailed = 202,
  kInvalidateFailed = 203,
  kViewRegistrationFailed = 204,

  // System errors (300-399)
  kSystemInitFailed = 300,
  kSystemCallFailed = 301,

  // Unknown
  kUnknown = 999
};

// Convert ErrorCode to short, stable English text. The returned string is a
// static literal and does not require lifetime management.
inline const char* ErrorCodeToString(ErrorCode code) {
  switch (code) {
    case ErrorCode::kSuccess:
      return "Success";
    case ErrorCode::kInvalidArgument:
      return "Invalid argument";
    case ErrorCode::kOutOfRange:
      return "Out of range";
    case ErrorCode::kNotInitialized:
      return "Not initialized";
    case ErrorCode::kAlreadyInitialized:
      return "Already initialized";
    case ErrorCode::kAllocationFailed:
      return "Memory allocation failed";
    case ErrorCode::kMemoryTooLarge:
      return "Memory size too large";
    case ErrorCode::kNoAllocation:
      return "No allocation";
    case ErrorCode::kNotOwned:
      return "Memory not owned";
    case ErrorCode::kReferencesRemain:
      return "References remain";
    case ErrorCode::kMemFreeFailed:
      return "Memory free failed";
    case ErrorCode::kMapFailed:
      return "Memory mapping failed";
    case ErrorCode::kUnmapFailed:
      return "Memory unmapping failed";
    case ErrorCode::kFlushFailed:
      return "Cache flush failed";
    case ErrorCode::kInvalidateFailed:
      return "Cache invalidate failed";
    case ErrorCode::kViewRegistrationFailed:
      return "View registration failed";
    case ErrorCode::kSystemInitFailed:
      return "System initialization failed";
    case ErrorCode::kSystemCallFailed:
      return "System call failed";
    case ErrorCode::kUnknown:
    default:
      return "Unknown error";
  }
}

}  // namespace axsys

