#ifndef SYMBOL_EXPORTS_H_
#define SYMBOL_EXPORTS_H_

// Platform-specific macros for exporting and importing symbols
#if defined(_WIN32) || defined(_WIN64)
// Importing symbols from DLL
#define EXPORT_C __declspec(dllimport)
#else
// For non-Windows platforms, control visibility
#define EXPORT_C
#endif

#endif  // SYMBOL_EXPORTS_H_