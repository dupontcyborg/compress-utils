#ifndef SYMBOL_EXPORTS_HPP_
#define SYMBOL_EXPORTS_HPP_

// Platform-specific macros for exporting and importing symbols
#if defined(_WIN32) || defined(_WIN64)
#if defined(COMPRESS_UTILS_EXPORT_STATIC)
// Static library, no need for dllimport/dllexport
#define EXPORT
#elif defined(COMPRESS_UTILS_EXPORT_SHARED)
// Exporting symbols for DLL
#define EXPORT __declspec(dllexport)
#else
// Importing symbols from DLL
#define EXPORT __declspec(dllimport)
#endif
#else
// For non-Windows platforms, control visibility
#define EXPORT __attribute__((visibility("default")))
#endif

#endif // SYMBOL_EXPORTS_HPP_