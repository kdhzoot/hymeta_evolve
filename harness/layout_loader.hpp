#pragma once

#include <string>
#include <vector>

#include "sim_engine.hpp"

namespace hymeta {

// Minimal ad-hoc JSON reader for the SST layout format produced by
// parse_sst_layout.py: a flat array of objects with keys
// {level, file_num, size, min_key, max_key}. Not a general JSON parser.
// `size` (bytes) is converted to num_keys using (key_size + value_size).
std::vector<SSTFile> load_sst_layout_json(const std::string& path,
                                           int key_size, int value_size);

}  // namespace hymeta
