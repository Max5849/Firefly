#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <array>
#include <set>
#include <algorithm>
#include <numeric>
#include <random>
#include <cassert>
#include <atomic>
#include <thread>
#include <barrier>
#include <mutex>
#include <bit>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <system_error>
#include <sys/mman.h>
#include "solver/include/ankerl/unordered_dense.h"

// A stage sets how coarse the metas or the memory are: stage 0 is the full detail and
// each stage above it is simpler. A hand-analysis table built for stage s carries the
// letter 'A' + s, so stage 0 reads handMetasA.bin and stage 1 handMetasB.bin.
inline std::string stagePath(const std::string& name, int stage) {
    return "solver/handAnalysis/" + name + char('A' + stage) + ".bin";
}
