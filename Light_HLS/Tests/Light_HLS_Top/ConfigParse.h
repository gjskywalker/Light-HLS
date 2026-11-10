#ifndef _Gobal_Confic_Parse
#define _Gobal_Confic_Parse

#include "HI_StringProcess.h"
#include "HI_SysExec.h"
#include "HI_print.h"
#include <assert.h>
#include <iostream>
#include <map>
#include <string>

extern std::string clock_period_str;
extern std::string HLS_lib_path;

struct MemoryInterfaceConfig
{
    unsigned ports = 1;
    unsigned readLatency = 1;
};

struct StorageBindingConfig
{
    std::string type;
    std::string implementation;
    unsigned readLatency = 1;
};

void Parse_Config(const char *config_file_name, std::map<std::string, int> &LoopLabel2UnrollFactor,
                  std::map<std::string, int> &LoopLabel2II);

bool BuildEffectiveConfigFromSourcePragmas(const std::string &rewritten_source_file,
                                           const std::string &original_source_file,
                                           const std::string &top_function,
                                           const std::string &base_config_file,
                                           const std::string &requested_board,
                                           const std::string &requested_clock_period,
                                           const std::string &output_config_file,
                                           unsigned &source_pragma_count,
                                           std::string &selected_board,
                                           std::string &selected_library_path,
                                           std::string &error_message);

bool ParseMemoryDepthConfig(
    const std::string &config_file, const std::string &top_function,
    std::map<std::string, unsigned long long> &memory_depths,
    std::string &error_message);

bool ParseMemoryInterfaceConfig(
    const std::string &config_file, const std::string &top_function,
    std::map<std::string, MemoryInterfaceConfig> &memory_interfaces,
    std::string &error_message);

bool ParseStorageBindingConfig(
    const std::string &config_file, const std::string &top_function,
    std::map<std::string, StorageBindingConfig> &storage_bindings,
    std::string &error_message);

void parseArrayPartition(std::stringstream &iss);

void parseLoopUnroll(std::stringstream &iss, std::map<std::string, int> &LoopLabel2UnrollFactor);

void parseLoopPipeline(std::stringstream &iss, std::map<std::string, int> &LoopLabel2II);

void parseArrayPortNum(std::stringstream &iss);

void parseFuncDataflow(std::stringstream &iss);

void parseLocalArray(std::stringstream &iss);

void parseResourceLimit(std::stringstream &iss);

#endif
