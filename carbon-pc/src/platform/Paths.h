// Maps Switch mount paths to host paths: "rom:/x" -> <romfs dir>/x, "save:/x" -> <save dir>/x.
#pragma once
#include <string>

namespace Paths {
void SetRomRoot(const std::string& dir);
void SetSaveRoot(const std::string& dir);
std::string Resolve(const std::string& path);
}  // namespace Paths
