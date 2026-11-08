#pragma once
#include <string>
void initialize_paths(const std::string& executable);
std::string resource_path(const std::string& relative);
std::string data_path(const std::string& relative);

std::wstring getpathU(const char* dir, const char* file);
std::string getpathM(const char* dir, const char* file);
