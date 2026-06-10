#pragma once

#include <string>

enum class FormatProfile { A_decimal, B_testno };

int decimal_depth(const std::string& number);
bool is_test_number(const std::string& s);
int level_for(FormatProfile profile, const std::string& number, bool in_test_scope);
int retrieval_depth(FormatProfile profile);
FormatProfile profile_from_string(const std::string& s);
std::string profile_to_string(FormatProfile p);
