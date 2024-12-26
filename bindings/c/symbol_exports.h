#ifndef SYMBOL_EXPORTS_H_
#define SYMBOL_EXPORTS_H_

// Platform-specific macros for exporting and importing symbols
#if defined(_WIN32) || defined(_WIN64)
#if defined(COMPRESS_UTILS_C_EXPORT_STATIC)
// Static library, no need for dllimport/dllexport
#define EXPORT_C
#elif defined(COMPRESS_UTILS_C_EXPORT_SHARED)
// Exporting symbols for DLL
#define EXPORT_C __declspec(dllexport)
#else
// Importing symbols from DLL
#define EXPORT_C __declspec(dllimport)
#endif
#else
// For non-Windows platforms, control visibility
#define EXPORT_C __attribute__((visibility("default")))
#endif

#endif  // SYMBOL_EXPORTS_H_