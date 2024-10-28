#ifndef SYMBOL_EXPORTS_HPP_
#define SYMBOL_EXPORTS_HPP_

// Platform-specific macros for exporting and importing symbols
#if defined(_WIN32) || defined(_WIN64)
// Importing symbols from DLL
#define EXPORT __declspec(dllimport)
#else
// For non-Windows platforms, control visibility
#define EXPORT
#endif

#endif // SYMBOL_EXPORTS_HPP_