#include "execpath.h"
#include <string>
#include <filesystem>
#if defined(_WIN32)
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <limits.h>
#elif defined(__linux__)
    #include <unistd.h>
    #include <limits.h>
#endif

std::string getExecutionPath() {
    char buffer[PATH_MAX];

#if defined(_WIN32)
    GetModuleFileNameA(NULL, buffer, PATH_MAX);
    std::string path(buffer);
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        path = path.substr(0, lastSlash);
        return path + "\\";
    }
    return "";

#elif defined(__APPLE__)
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        std::string path(buffer);
        size_t lastSlash = path.find_last_of("/");
        if (lastSlash != std::string::npos) {
            path = path.substr(0, lastSlash);
            return path + "/";
        }
        return "";
    } else
        return "";  // Buffer too small, but shouldn't happen with PATH_MAX

#elif defined(__linux__)
    ssize_t count = readlink("/proc/self/exe", buffer, PATH_MAX);
    if (count != -1) {
        std::string path(buffer, count);
        size_t lastSlash = path.find_last_of("/");
        if (lastSlash != std::string::npos) {
            path = path.substr(0, lastSlash);
            return path + "/";
        }
        return "";
    } else
        return "";
#else
    return "";
#endif
} 