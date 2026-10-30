#pragma once
#include <string_view>

// _StringProcessing's Regex, enough for regex literals: a backtracking matcher over UTF-8 bytes.
void* regex_symbol(std::string_view name);
