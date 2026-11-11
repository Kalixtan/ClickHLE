#pragma once

#include "clickhle/common/types.h"
#include <memory>

namespace clickhle::vc2 {

class Core;

// Optional native translator. A zero result asks Core to execute one
// instruction in the reference interpreter.
class Jit {
  public:
    Jit();
    ~Jit();
    Jit(const Jit &) = delete;
    Jit &operator=(const Jit &) = delete;

    unsigned try_execute(Core &core, unsigned limit, u16 first_word);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace clickhle::vc2
