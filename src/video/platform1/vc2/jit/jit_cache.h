#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

namespace llvm { class MemoryBuffer; }

namespace clickhle::vc2 {

class JitObjectCache {
  public:
    JitObjectCache();
    std::unique_ptr<llvm::MemoryBuffer> load(std::string_view key) const;
    void store(std::string_view key, const llvm::MemoryBuffer &object) const;

  private:
    std::filesystem::path directory_;
};

} // namespace clickhle::vc2
