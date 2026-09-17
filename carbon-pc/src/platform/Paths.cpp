#include "platform/Paths.h"

namespace Paths {
namespace {

std::string romRoot = "romfs";
std::string saveRoot = "save";

std::string Join(const std::string& root, const std::string& rest) {
    size_t start = rest.find_first_not_of('/');
    if (start == std::string::npos) return root;
    return root + "/" + rest.substr(start);
}

}  // namespace

void SetRomRoot(const std::string& dir) { romRoot = dir; }
void SetSaveRoot(const std::string& dir) { saveRoot = dir; }

std::string Resolve(const std::string& path) {
    if (path.rfind("rom:/", 0) == 0) return Join(romRoot, path.substr(5));
    if (path.rfind("save:/", 0) == 0) return Join(saveRoot, path.substr(6));
    return path;
}

}  // namespace Paths
