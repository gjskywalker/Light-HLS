#include "HI_InstructionFiles.h"
#include "HI_WithDirectiveTimingResourceEvaluation.h"
#include <fstream>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace llvm;

// Pass for simple evluation of the latency of the top function, without considering HLS directives
void HI_WithDirectiveTimingResourceEvaluation::Parse_Config_fromFile()
{
    std::string tmp_s;
    std::string tmpStr_forParsing;
    while (getline(*config_file, tmp_s))
    {
        tmp_s = removeExtraSpace(tmp_s);
        std::stringstream iss(tmp_s);
        std::string param_name;
        iss >> param_name; //  get the name of parameter

        switch (hash_(param_name.c_str()))
        {
        case hash_compile_time("clock"):
            consumeEqual(iss);
            iss >> clock_period_str;
            clock_period = std::stod(clock_period_str);
            break;

        case hash_compile_time("HLS_lib_path"):
            consumeEqual(iss);
            iss >> HLS_lib_path;
            break;

        case hash_compile_time("array_partition"):
            parseArrayPartition(iss);
            break;

        case hash_compile_time("loop_pipeline"):
            parseLoopPipeline(iss);
            break;

        case hash_compile_time("loop_unroll"):
            parseLoopUnroll(iss);
            break;

        case hash_compile_time("array_port"):
            parseArrayPortNum(iss);
            break;

        case hash_compile_time("func_dataflow"):
            parseFuncDataflow(iss);
            break;

        case hash_compile_time("local_array"):
            parseLocalArray(iss);
            break;

        default:
            break;
        }
    }
    assert(HLS_lib_path != "" && "The HLS Lib is necessary in the configuration file!\n");
}

void HI_WithDirectiveTimingResourceEvaluation::Parse_Config_toInfo(HI_DesignConfigInfo &desginconfig)
{
    std::string tmp_s;
    std::string tmpStr_forParsing;
    while (getline(*config_file, tmp_s))
    {
        tmp_s = removeExtraSpace(tmp_s);
        std::stringstream iss(tmp_s);
        std::string param_name;
        iss >> param_name; //  get the name of parameter

        switch (hash_(param_name.c_str()))
        {
        case hash_compile_time("clock"):
            consumeEqual(iss);
            iss >> clock_period_str;
            desginconfig.setClock(clock_period_str);
            // clock_period = std::stod(clock_period_str);
            break;

        case hash_compile_time("HLS_lib_path"):
            consumeEqual(iss);
            iss >> HLS_lib_path;
            desginconfig.HLS_lib_path = HLS_lib_path;
            break;

        case hash_compile_time("array_partition"):
            parseArrayPartition(iss, desginconfig);
            break;

        case hash_compile_time("loop_unroll"):
            parseLoopUnroll(iss, desginconfig);
            break;

        case hash_compile_time("loop_pipeline"):
            parseLoopPipeline(iss, desginconfig);
            break;

        case hash_compile_time("array_port"):
            parseArrayPortNum(iss, desginconfig);
            break;

        case hash_compile_time("func_dataflow"):
            parseFuncDataflow(iss, desginconfig);
            break;

        case hash_compile_time("stream"):
            parseStream(iss, desginconfig);
            break;

        case hash_compile_time("local_array"):
            parseLocalArray(iss, desginconfig);
            break;

        default:
            break;
        }
    }
    assert(HLS_lib_path != "" && "The HLS Lib is necessary in the configuration file!\n");
}

// parse the argument for array partitioning
void HI_WithDirectiveTimingResourceEvaluation::parseArrayPartition(std::stringstream &iss,
                                                                   HI_DesignConfigInfo &desginconfig)
{
    std::string targetStr, scopeStr;
    int dim, partition_factor;
    bool cyclic = 1;
    bool complete = 0;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("variable"):
            consumeEqual(iss);
            iss >> tmp_val;
            targetStr = (tmp_val);
            break;

        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            scopeStr = (tmp_val);
            break;

        case hash_compile_time("dim"):
            consumeEqual(iss);
            iss >> tmp_val;
            dim = std::stoi(tmp_val); // count from dim="0" to match the storage format
            break;

        case hash_compile_time("factor"):
            consumeEqual(iss);
            iss >> tmp_val;
            partition_factor = std::stoi(tmp_val);
            break;

        case hash_compile_time("cyclic"):
            cyclic = 1;
            break;

        case hash_compile_time("block"):
            cyclic = 0;
            break;

        case hash_compile_time("complete"):
            complete = 1;
            break;

        default:
            print_error("Wrong argument for array partitioning.");
            break;
        }
    }
    if (complete)
    {
        desginconfig.insertArrayCompletePartition(scopeStr, targetStr, dim);
    }
    else if (cyclic)
    {
        desginconfig.insertArrayCyclicPartition(scopeStr, targetStr, dim, partition_factor);
    }
    else
    {
        desginconfig.insertArrayblockPartition(scopeStr, targetStr, dim, partition_factor);
    }
}

// parse the argument for array port num setting
void HI_WithDirectiveTimingResourceEvaluation::parseArrayPortNum(std::stringstream &iss,
                                                                 HI_DesignConfigInfo &desginconfig)
{
    std::string targetStr, scopeStr;
    int port_num;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("variable"):
            consumeEqual(iss);
            iss >> tmp_val;
            targetStr = (tmp_val);
            break;

        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            scopeStr = (tmp_val);
            break;

        case hash_compile_time("port_num"):
            consumeEqual(iss);
            iss >> tmp_val;
            port_num = std::stoi(tmp_val); // count from dim="0" to match the storage format
            break;

        default:
            llvm::errs() << "wrong argument: " << arg_name << "\n";
            print_error("Wrong argument for array port num setting.");
            break;
        }
    }

    desginconfig.insertArrayPortNum(scopeStr, targetStr, port_num);
}

// parse the argument for function dataflow setting
void HI_WithDirectiveTimingResourceEvaluation::parseFuncDataflow(std::stringstream &iss,
                                                                 HI_DesignConfigInfo &desginconfig)
{
    std::string targetStr, scopeStr;
    int port_num;
    bool enable = false;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {

        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            scopeStr = (tmp_val);
            break;

        case hash_compile_time("enable"):
            enable = true;
            break;

        case hash_compile_time("disable"):
            enable = false;
            break;

        default:
            llvm::errs() << "wrong argument: " << arg_name << "\n";
            print_error("Wrong argument for array port num setting.");
            break;
        }
    }

    desginconfig.insertFuncDataflow(scopeStr, enable);
}

void HI_WithDirectiveTimingResourceEvaluation::parseStream(
    std::stringstream &iss, HI_DesignConfigInfo &desginconfig)
{
    std::string variable, scope, type = "fifo";
    int depth = 2;
    int bitwidth = 0;
    int instances = 1;
    int initialTokens = 0;
    std::string arg_name;
    while (iss >> arg_name)
    {
        std::string tmp_val;
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("variable"):
            consumeEqual(iss);
            iss >> variable;
            break;
        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> scope;
            break;
        case hash_compile_time("type"):
            consumeEqual(iss);
            iss >> type;
            break;
        case hash_compile_time("depth"):
            consumeEqual(iss);
            iss >> tmp_val;
            depth = std::stoi(tmp_val);
            break;
        case hash_compile_time("bitwidth"):
            consumeEqual(iss);
            iss >> tmp_val;
            bitwidth = std::stoi(tmp_val);
            break;
        case hash_compile_time("instances"):
            consumeEqual(iss);
            iss >> tmp_val;
            instances = std::stoi(tmp_val);
            break;
        case hash_compile_time("initial_tokens"):
            consumeEqual(iss);
            iss >> tmp_val;
            initialTokens = std::stoi(tmp_val);
            break;
        default:
            print_error("Wrong argument for stream setting.");
            break;
        }
    }
    desginconfig.insertStream(scope, variable, type, depth, bitwidth,
                              instances, initialTokens);
}

// parse the argument for local array setting
void HI_WithDirectiveTimingResourceEvaluation::parseLocalArray(std::stringstream &iss,
                                                               HI_DesignConfigInfo &desginconfig)
{
    std::string targetStr, scopeStr;
    bool enable = false;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;

        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("variable"):
            consumeEqual(iss);
            iss >> tmp_val;
            targetStr = (tmp_val);
            break;

        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            scopeStr = (tmp_val);
            break;

        case hash_compile_time("enable"):
            enable = true;
            break;

        case hash_compile_time("disable"):
            enable = false;
            break;

        default:
            llvm::errs() << "wrong argument: " << arg_name << "\n";
            print_error("Wrong argument for local array setting.");
            break;
        }
    }

    desginconfig.insertLocalArray(scopeStr, targetStr, enable);
}

// parse the argument for loop unrolling
void HI_WithDirectiveTimingResourceEvaluation::parseLoopUnroll(std::stringstream &iss,
                                                               HI_DesignConfigInfo &desginconfig)
{
    int factor = -1;
    std::string loopLabel("");
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("label"):
            consumeEqual(iss);
            iss >> tmp_val;
            loopLabel = (tmp_val);
            break;

        case hash_compile_time("factor"):
            consumeEqual(iss);
            iss >> tmp_val;
            factor = std::stoi(tmp_val);
            break;

        default:
            print_error("Wrong argument for loop unrolling.");
            break;
        }
    }
    assert(loopLabel != "" && factor > -1);
    desginconfig.insertLoopUnroll(loopLabel, factor);
}

// parse the argument for loop pipelining
void HI_WithDirectiveTimingResourceEvaluation::parseLoopPipeline(std::stringstream &iss,
                                                                 HI_DesignConfigInfo &desginconfig)
{
    int factor = -1;
    std::string loopLabel("");
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("label"):
            consumeEqual(iss);
            iss >> tmp_val;
            loopLabel = (tmp_val);
            break;

        case hash_compile_time("II"):
            consumeEqual(iss);
            iss >> tmp_val;
            factor = std::stoi(tmp_val);
            break;

        default:
            print_error("Wrong argument for loop pipelining.");
            break;
        }
    }
    assert(loopLabel != "" && factor > -1);
    desginconfig.insertLoopPipeline(loopLabel, factor);
}

// Pass for simple evluation of the latency of the top function, without considering HLS directives
void HI_WithDirectiveTimingResourceEvaluation::Parse_Config(const HI_DesignConfigInfo &configInfo)
{
    std::string tmp_s;
    std::string tmpStr_forParsing;

    // get clock
    clock_period_str = configInfo.clock_period_str;
    clock_period = std::stod(clock_period_str);
    HLS_lib_path = configInfo.HLS_lib_path;

    // get loop unroll configs
    for (auto loop_unroll_pair : configInfo.loopUnrollConfigs)
    {
        HI_PragmaInfo ans_pragma;
        ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::loopUnroll_Pragma;
        ans_pragma.labelStr = loop_unroll_pair.first;
        ans_pragma.unroll_factor = loop_unroll_pair.second;
        PragmaInfo_List.push_back(ans_pragma);
    }

    // get loop pipeline configs
    for (auto loop_pipeline_pair : configInfo.loopPipelineConfigs)
    {
        HI_PragmaInfo ans_pragma;
        ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::loopPipeline_Pragma;
        ans_pragma.labelStr = loop_pipeline_pair.first;
        ans_pragma.II = loop_pipeline_pair.second;
        PragmaInfo_List.push_back(ans_pragma);
    }

    // get array cyclic partition configs
    for (auto cyclic_partition_pair : configInfo.cyclicPartitionConfigs)
    {
        // (std::string functionName, std::string arrayName, int dim, int factor)
        HI_PragmaInfo ans_pragma;
        ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::arrayPartition_Pragma;
        ans_pragma.scopeStr = cyclic_partition_pair.first;
        ans_pragma.targetStr = cyclic_partition_pair.second.first;
        ans_pragma.dim = cyclic_partition_pair.second.second.first - 1;
        ans_pragma.partition_factor = cyclic_partition_pair.second.second.second;
        ans_pragma.cyclic = 1;
        PragmaInfo_List.push_back(ans_pragma);
    }

    // get array block partition configs
    for (auto block_partition_pair : configInfo.blockPartitionConfigs)
    {
        // (std::string functionName, std::string arrayName, int dim, int factor)
        HI_PragmaInfo ans_pragma;
        ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::arrayPartition_Pragma;
        ans_pragma.scopeStr = block_partition_pair.first;
        ans_pragma.targetStr = block_partition_pair.second.first;
        ans_pragma.dim = block_partition_pair.second.second.first - 1;
        ans_pragma.partition_factor = block_partition_pair.second.second.second;
        ans_pragma.cyclic = 0;
        PragmaInfo_List.push_back(ans_pragma);
    }

    // get array complete partition configs
    for (auto complete_partition_pair : configInfo.completePartitionConfigs)
    {
        // (std::string functionName, std::string arrayName, int dim, int factor)
        HI_PragmaInfo ans_pragma;
        ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::arrayPartition_Pragma;
        ans_pragma.scopeStr = complete_partition_pair.first;
        ans_pragma.targetStr = complete_partition_pair.second.first;
        ans_pragma.dim = complete_partition_pair.second.second - 1;
        ans_pragma.cyclic = 1;
        ans_pragma.complete = 1;
        PragmaInfo_List.push_back(ans_pragma);
    }

    // get array port configs
    for (auto port_num_pair : configInfo.arrayPortConfigs)
    {
        HI_PragmaInfo ans_pragma;
        ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::arrayPortNum_Pragma;
        ans_pragma.scopeStr = port_num_pair.first;
        ans_pragma.targetStr = port_num_pair.second.first;
        ans_pragma.port_num = port_num_pair.second.second;
        PragmaInfo_List.push_back(ans_pragma);
    }

    // get function dataflow configs
    for (auto func_dataflow_pair : configInfo.funcDataflowConfigs)
    {
        HI_PragmaInfo ans_pragma;
        ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::funcDataflow_Pragma;
        ans_pragma.scopeStr = func_dataflow_pair.first;
        ans_pragma.dataflowEnable = func_dataflow_pair.second;
        PragmaInfo_List.push_back(ans_pragma);
    }

    // get local array configs
    for (auto local_set_pair : configInfo.localArrayConfigs)
    {
        HI_PragmaInfo ans_pragma;
        ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::localArray_Pragma;
        ans_pragma.scopeStr = local_set_pair.first;
        ans_pragma.targetStr = local_set_pair.second.first;
        ans_pragma.localArrayEnable = local_set_pair.second.second;
        PragmaInfo_List.push_back(ans_pragma);
    }

    assert(HLS_lib_path != "" && "The HLS Lib is necessary in the configuration file!\n");
}

// load the HLS database of timing and resource
void HI_WithDirectiveTimingResourceEvaluation::Load_Instruction_Info()
{
    int i;
    for (i = 0; i < instructionInfoNum; i++)
    {
        if (instructionHasMappingFile[i])
        {
            Info_type_list tmp_list;
            std::string info_file_name(HLS_lib_path.c_str());
            info_file_name += instructionNames[i];
            info_file_name += "/";
            info_file_name += instructionNames[i];
            // info_file_name+="_data";
            std::ifstream info_file(info_file_name.c_str());

            if (!exists_test(info_file_name))
            {
                llvm::errs() << "The HLS info file [" + info_file_name + "] does not exist.\n";
                assert(false && "check the HLS information library path.\n");
            }
            std::string tmp_s;
            while (getline(info_file, tmp_s))
            {
                tmp_s = removeExtraSpace(tmp_s);
                std::stringstream iss(tmp_s);
                std::string data_ele[11];
                for (int j = 0; j < 11; j++)
                {
                    iss >> data_ele[j];
                    iss.ignore(1, ' ');
                }
                if (instructionNames[i] != "mac")
                {
                    if (data_ele[0] != data_ele[1] || data_ele[1] != data_ele[2])
                        continue; // ignore those unused information
                }
                else
                {
                    if (data_ele[0] != data_ele[1])
                        continue; // ignore those unused information
                }

                inst_timing_resource_info tmp_info;

                int oprand_bitwidth = std::stoi(data_ele[0]);
                int res_bitwid = std::stoi(data_ele[2]);
                std::string _clock_period_str = data_ele[3];
                tmp_info.DSP = std::stoi(data_ele[4]);
                tmp_info.FF = std::stoi(data_ele[5]);
                tmp_info.LUT = std::stoi(data_ele[6]);
                tmp_info.Lat = std::stoi(data_ele[7]);
                tmp_info.delay = std::stod(data_ele[8]);
                tmp_info.II = std::stoi(data_ele[9]);
                tmp_info.core_name = data_ele[10];
                tmp_list[oprand_bitwidth][res_bitwid][_clock_period_str] = tmp_info;
                // str(oprandA)+" "+str(oprandB)+" "+str(oprandC)+" "+str(period)+" "
                // +str(DSP48E_N)+" "+str(FF_N)+" "+str(LUT_N)+" "+str(lat_tmp)+" "
                // +str(delay_tmp)+" "+str(II_tmp)+" "+str(Core_tmp)
            }
            BiOp_Info_name2list_map[instructionNames[i]] = tmp_list;
        }
    }
}

// Organize the information into timingBase after getting the information of a specific instruction,
// based on its opcode, operand_bitwidth, result_bitwidth and clock period.
HI_WithDirectiveTimingResourceEvaluation::timingBase
HI_WithDirectiveTimingResourceEvaluation::get_inst_TimingInfo_result(std::string opcode, int operand_bitwid,
                                                                     int res_bitwidth, std::string period)
{
    timingBase result(0, 0, 1, clock_period);
    inst_timing_resource_info info = get_inst_info(opcode, operand_bitwid, res_bitwidth, clock_period_str);
    result.latency = info.Lat;
    result.timing = info.delay;
    result.II = info.II;
    // result.strict_timing = "store"==opcode;  // TODO: turn the strict timing option into library
    return result;
}

// Organize the information into resourceBase after getting the information of a specific
// instruction, based on its opcode, operand_bitwidth, result_bitwidth and clock period.
HI_WithDirectiveTimingResourceEvaluation::resourceBase
HI_WithDirectiveTimingResourceEvaluation::get_inst_ResourceInfo_result(std::string opcode, int operand_bitwid,
                                                                       int res_bitwidth, std::string period)
{
    resourceBase result(0, 0, 0, clock_period);
    inst_timing_resource_info info = get_inst_info(opcode, operand_bitwid, res_bitwidth, clock_period_str);
    result.DSP = info.DSP;
    result.FF = info.FF;
    result.LUT = info.LUT;
    return result;
}

// get the information of a specific instruction, based on its opcode, operand_bitwidth,
// result_bitwidth and clock period
inst_timing_resource_info HI_WithDirectiveTimingResourceEvaluation::get_inst_info(std::string opcode,
                                                                                  int operand_bitwid, int res_bitwidth,
                                                                                  std::string period)
{
    if (checkInfoAvailability(opcode, operand_bitwid, res_bitwidth, period))
        return BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period];
    if (checkFreqProblem(opcode, operand_bitwid, res_bitwidth, period))
    {
        inst_timing_resource_info info =
            checkInfo_HigherFreq(opcode, operand_bitwid, res_bitwidth, period);
        BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period] = info;
        return info;
    }

    auto opcode_it = BiOp_Info_name2list_map.find(opcode);
    if (opcode_it != BiOp_Info_name2list_map.end() && !opcode_it->second.empty())
    {
        auto operand_it = opcode_it->second.lower_bound(operand_bitwid);
        if (operand_it == opcode_it->second.end())
            --operand_it;
        if (!operand_it->second.empty())
        {
            auto result_it = operand_it->second.lower_bound(res_bitwidth);
            if (result_it == operand_it->second.end())
                --result_it;
            inst_timing_resource_info info =
                get_inst_info(opcode, operand_it->first, result_it->first, period);
            BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period] = info;
            return info;
        }
    }

    llvm::errs() << "inquirying : " << opcode << " -- " << operand_bitwid
                 << " -- " << res_bitwidth << " -- " << period << " \n";
    assert(false && "no such information in the database\n");
}

// check whether a specific information is in the database
bool HI_WithDirectiveTimingResourceEvaluation::checkInfoAvailability(std::string opcode, int operand_bitwid,
                                                                     int res_bitwidth, std::string period)
{
    if (BiOp_Info_name2list_map.find(opcode) != BiOp_Info_name2list_map.end())
    {
        if (BiOp_Info_name2list_map[opcode].find(operand_bitwid) != BiOp_Info_name2list_map[opcode].end())
        {
            if (BiOp_Info_name2list_map[opcode][operand_bitwid].find(res_bitwidth) !=
                BiOp_Info_name2list_map[opcode][operand_bitwid].end())
            {
                if (BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth].find(period) !=
                    BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth].end())
                {
                    return true;
                }
                else
                {
                    // llvm::errs() << "not in
                    // BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth]\n";
                }
            }
            else
            {
                // llvm::errs() << "not in BiOp_Info_name2list_map[opcode][operand_bitwid]\n";
            }
        }
        else
        {
            // llvm::errs() << "not in BiOp_Info_name2list_map[opcode]\n";
        }
    }
    else
    {
        // llvm::errs() << "not in BiOp_Info_name2list_map\n";
    }

    return false;
}

// check whether we can infer the information by increasing the clock frequency
bool HI_WithDirectiveTimingResourceEvaluation::checkFreqProblem(std::string opcode, int operand_bitwid,
                                                                int res_bitwidth, std::string period)
{
    if (BiOp_Info_name2list_map.find(opcode) != BiOp_Info_name2list_map.end())
    {
        if (BiOp_Info_name2list_map[opcode].find(operand_bitwid) != BiOp_Info_name2list_map[opcode].end())
        {
            if (BiOp_Info_name2list_map[opcode][operand_bitwid].find(res_bitwidth) !=
                BiOp_Info_name2list_map[opcode][operand_bitwid].end())
            {
                return true;
            }
            else
            {
                // llvm::errs() << "not in BiOp_Info_name2list_map[opcode][operand_bitwid]\n";
            }
        }
        else
        {
            // llvm::errs() << "not in BiOp_Info_name2list_map[opcode]\n";
        }
    }
    else
    {
        // llvm::errs() << "not in BiOp_Info_name2list_map\n";
    }

    return false;
}

// if the information is not found in database, we may infer the information by increasing the clock
// frequency
inst_timing_resource_info HI_WithDirectiveTimingResourceEvaluation::checkInfo_HigherFreq(std::string opcode,
                                                                                         int operand_bitwid,
                                                                                         int res_bitwidth,
                                                                                         std::string period)
{
    const double requestedPeriod = std::stod(period);
    int selected = -1;
    double selectedPeriod = -1;
    // Arbitrary user frequencies need not coincide with library grid points.
    // Select the nearest characterized period no slower than the request.
    // Never extrapolate an uncharacterized faster clock from a slower entry.
    for (int index = 0; index < clockNum; ++index)
    {
        const double candidate = std::stod(clockStrs[index]);
        if (candidate <= requestedPeriod + 1e-9 && candidate > selectedPeriod &&
            checkInfoAvailability(opcode, operand_bitwid, res_bitwidth, clockStrs[index]))
        {
            selected = index;
            selectedPeriod = candidate;
        }
    }
    if (selected >= 0)
    {
        const auto information = BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][clockStrs[selected]];
        BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period] = information;
        return information;
    }
    llvm::errs() << "HLS_LIBRARY_CLOCK_UNSUPPORTED: no characterized " << opcode
                 << " (" << operand_bitwid << " -> " << res_bitwidth
                 << ") at or below period " << period << " ns.\n";
    std::exit(EXIT_FAILURE);
}

// parse the argument for array partitioning
void HI_WithDirectiveTimingResourceEvaluation::parseArrayPartition(std::stringstream &iss)
{
    HI_PragmaInfo ans_pragma;
    ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::arrayPartition_Pragma;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("variable"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.targetStr = (tmp_val);
            break;

        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.scopeStr = (tmp_val);
            break;

        case hash_compile_time("dim"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.dim = std::stoi(tmp_val) - 1; // count from dim="0" to match the storage format
            break;

        case hash_compile_time("factor"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.partition_factor = std::stoi(tmp_val);
            break;

        case hash_compile_time("cyclic"):
            ans_pragma.cyclic = 1;
            break;

        case hash_compile_time("block"):
            ans_pragma.cyclic = 0;
            break;

        case hash_compile_time("complete"):
            ans_pragma.complete = 1;
            break;

        default:
            llvm::errs() << "wrong argument: " << arg_name << "\n";
            print_error("Wrong argument for array partitioning.");
            break;
        }
    }
    llvm::errs() << "partition array: " << ans_pragma.targetStr << " with dim=" << ans_pragma.dim + 1
                 << " and factor=" << ans_pragma.partition_factor << "\n";
    PragmaInfo_List.push_back(ans_pragma);
    //
}

// parse the argument for  array port number setting
void HI_WithDirectiveTimingResourceEvaluation::parseArrayPortNum(std::stringstream &iss)
{
    HI_PragmaInfo ans_pragma;
    ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::arrayPortNum_Pragma;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("variable"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.targetStr = (tmp_val);
            break;

        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.scopeStr = (tmp_val);
            break;

        case hash_compile_time("port_num"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.port_num = std::stoi(tmp_val); // count from dim="0" to match the storage format
            break;

        default:
            llvm::errs() << "wrong argument: " << arg_name << "\n";
            print_error("Wrong argument for array port num setting.");
            break;
        }
    }
    PragmaInfo_List.push_back(ans_pragma);
    //
}

// parse the argument for  function dataflow
void HI_WithDirectiveTimingResourceEvaluation::parseFuncDataflow(std::stringstream &iss)
{
    HI_PragmaInfo ans_pragma;
    ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::funcDataflow_Pragma;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.scopeStr = (tmp_val);
            break;

        case hash_compile_time("enable"):
            ans_pragma.dataflowEnable = true;
            break;

        case hash_compile_time("disable"):
            ans_pragma.dataflowEnable = false;
            break;

        default:
            llvm::errs() << "wrong argument: " << arg_name << "\n";
            print_error("Wrong argument for function dataflow setting.");
            break;
        }
    }
    PragmaInfo_List.push_back(ans_pragma);
}

// parse the argument for local array setting
void HI_WithDirectiveTimingResourceEvaluation::parseLocalArray(std::stringstream &iss)
{
    HI_PragmaInfo ans_pragma;
    ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::localArray_Pragma;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("variable"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.targetStr = (tmp_val);
            break;

        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.scopeStr = (tmp_val);
            break;

        case hash_compile_time("enable"):
            ans_pragma.localArrayEnable = true;
            break;

        case hash_compile_time("disable"):
            ans_pragma.localArrayEnable = true;
            break;

        default:
            llvm::errs() << "wrong argument: " << arg_name << "\n";
            print_error("Wrong argument for local array setting.");
            break;
        }
    }
    PragmaInfo_List.push_back(ans_pragma);
    //
}

// parse the argument for loop pipelining
void HI_WithDirectiveTimingResourceEvaluation::parseLoopPipeline(std::stringstream &iss)
{
    HI_PragmaInfo ans_pragma;
    ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::loopPipeline_Pragma;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("label"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.labelStr = (tmp_val);
            break;

        case hash_compile_time("II"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.II = std::stoi(tmp_val);
            break;

        default:
            llvm::errs() << "wrong argument: " << arg_name << "\n";
            if (ans_pragma.labelStr != "")
                llvm::errs() << " other info label=" << ans_pragma.labelStr << "\n";
            if (ans_pragma.II > 0)
                llvm::errs() << " other info II=" << ans_pragma.II << "\n";
            print_error("Wrong argument for loop pipelining.");
            break;
        }
    }
    PragmaInfo_List.push_back(ans_pragma);
}

// parse the argument for loop unrolling
void HI_WithDirectiveTimingResourceEvaluation::parseLoopUnroll(std::stringstream &iss)
{
    HI_PragmaInfo ans_pragma;
    ans_pragma.HI_PragmaInfoType = HI_PragmaInfo::loopUnroll_Pragma;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("label"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.labelStr = (tmp_val);
            break;

        case hash_compile_time("factor"):
            consumeEqual(iss);
            iss >> tmp_val;
            ans_pragma.unroll_factor = std::stoi(tmp_val);
            break;

        default:
            print_error("Wrong argument for loop unrolling.");
            break;
        }
    }
    PragmaInfo_List.push_back(ans_pragma);
}

// match the configuration and the corresponding declaration of memory (array)
void HI_WithDirectiveTimingResourceEvaluation::matchArrayAndConfiguration(Value *target, HI_ArrayInfo &resArrayInfo)
{
    // by default, we set the port number for BRAM to 1 (single port BRAM)
    resArrayInfo.port_num = 1;
    for (auto &pragma : PragmaInfo_List)
    {
        if (pragma.HI_PragmaInfoType == HI_PragmaInfo::arrayPartition_Pragma)
        {
            if (target->getName().str() == pragma.targetStr)
            {
                Function *F = getFunctionOfValue(target);
                std::string nameOfFunction =
                    F ? demangleFunctionName(F->getName().str()) : "";
                if (F && nameOfFunction != pragma.scopeStr)
                {
                    continue;
                }
                if (resArrayInfo.num_dims <= pragma.dim)
                {
                    continue;
                }
                bool validFactor = true;
                if (!pragma.complete)
                {
                    if (pragma.dim < 0)
                    {
                        for (int dim = 0; dim < resArrayInfo.num_dims; ++dim)
                            validFactor &= pragma.partition_factor <=
                                           resArrayInfo.dim_size[dim];
                    }
                    else
                    {
                        validFactor = pragma.partition_factor <=
                                      resArrayInfo.dim_size[pragma.dim];
                    }
                }
                if (!validFactor)
                    continue;
                pragma.targetArray = target;
                pragma.ScopeFunc = F;
                arrayDirectives[target].push_back(pragma);
                FuncArray2PartitionBenefit[std::pair<std::string, std::string>(nameOfFunction, pragma.targetStr)] =
                    false;

                if (pragma.dim < 0)
                {
                    assert(pragma.dim == -1); // partition all the dimension
                    for (int cur_dim = 0; cur_dim < resArrayInfo.num_dims; cur_dim++)
                    {
                        resArrayInfo.cyclic[cur_dim] = pragma.cyclic;
                        if (pragma.complete)
                        {
                            resArrayInfo.partition_size[cur_dim] = resArrayInfo.dim_size[cur_dim];
                        }
                        else
                            resArrayInfo.partition_size[cur_dim] = pragma.partition_factor;
                    }
                }
                else
                {
                    resArrayInfo.cyclic[pragma.dim] = pragma.cyclic;
                    if (pragma.complete)
                    {
                        resArrayInfo.partition_size[pragma.dim] = resArrayInfo.dim_size[pragma.dim];
                    }
                    else
                        resArrayInfo.partition_size[pragma.dim] = pragma.partition_factor;
                }
            }
        }
        else if (pragma.HI_PragmaInfoType == HI_PragmaInfo::arrayPortNum_Pragma)
        {
            if (target->getName().str() == pragma.targetStr)
            {
                Function *F = getFunctionOfValue(target);
                std::string nameOfFunction = demangleFunctionName(F->getName().str());
                assert(F && "the parent function of the value should be found.");
                if (nameOfFunction != pragma.scopeStr)
                {
                    continue;
                }
                if (resArrayInfo.num_dims <= pragma.dim)
                {
                    continue;
                }
                pragma.targetArray = target;
                pragma.ScopeFunc = F;
                arrayDirectives[target].push_back(pragma);
                resArrayInfo.port_num = pragma.port_num;
            }
        }
    }
}

bool HI_WithDirectiveTimingResourceEvaluation::validateArrayPartitionDirectives()
{
    bool valid = true;
    for (const auto &pragma : PragmaInfo_List)
    {
        if (pragma.HI_PragmaInfoType != HI_PragmaInfo::arrayPartition_Pragma ||
            pragma.targetArray)
        {
            continue;
        }

        const HI_ArrayInfo *matchingArray = nullptr;
        for (const auto &targetAndInfo : Target2ArrayInfo)
        {
            Value *target = targetAndInfo.first;
            if (!target || target->getName().str() != pragma.targetStr)
                continue;

            Function *F = getFunctionOfValue(target);
            if (F && demangleFunctionName(F->getName().str()) != pragma.scopeStr)
                continue;

            matchingArray = &targetAndInfo.second;
            break;
        }

        if (!matchingArray)
        {
            print_error("ARRAY_PARTITION target '" + pragma.targetStr +
                        "' was not found in function '" + pragma.scopeStr + "'.");
            valid = false;
            continue;
        }

        if (pragma.dim >= matchingArray->num_dims)
        {
            print_error("ARRAY_PARTITION dim=" + std::to_string(pragma.dim + 1) +
                        " is out of range for '" + pragma.targetStr + "' in function '" +
                        pragma.scopeStr + "'; the array has " +
                        std::to_string(matchingArray->num_dims) + " dimension(s).");
            valid = false;
            continue;
        }

        if (!pragma.complete)
        {
            int invalidDimension = -1;
            if (pragma.dim < 0)
            {
                for (int dim = 0; dim < matchingArray->num_dims; ++dim)
                {
                    if (pragma.partition_factor > matchingArray->dim_size[dim])
                    {
                        invalidDimension = dim;
                        break;
                    }
                }
            }
            else if (pragma.partition_factor >
                     matchingArray->dim_size[pragma.dim])
            {
                invalidDimension = pragma.dim;
            }

            if (invalidDimension >= 0)
            {
                print_error("ARRAY_PARTITION factor=" +
                            std::to_string(pragma.partition_factor) +
                            " exceeds dimension " +
                            std::to_string(invalidDimension + 1) + " size " +
                            std::to_string(matchingArray->dim_size[invalidDimension]) +
                            " for '" + pragma.targetStr + "' in function '" +
                            pragma.scopeStr + "'.");
                valid = false;
            }
        }
    }
    return valid;
}

// find which function the value is located in
Function *HI_WithDirectiveTimingResourceEvaluation::getFunctionOfValue(Value *target)
{

    if (auto I = dyn_cast<Instruction>(target))
    {
        return I->getParent()->getParent();
    }
    else if (auto A = dyn_cast<Argument>(target))
    {
        return A->getParent();
    }
    else
    {
        return nullptr;
    }
}

raw_ostream &operator<<(raw_ostream &stream, const HI_DesignConfigInfo &tb)
{

    for (auto unroll_pair : tb.loopUnrollConfigs)
    {
        stream << "    Loop: [" << unroll_pair.first << "] Unrolled with factor=" << unroll_pair.second << "\n";
    }
    for (auto pipeline_pair : tb.loopPipelineConfigs)
    {
        stream << "    Loop: [" << pipeline_pair.first << "] Pipelined with II=" << pipeline_pair.second << "\n";
    }
    for (auto partition_seq : tb.cyclicPartitionConfigs)
    {
        stream << "    Array: [" << partition_seq.second.first << "] in Function: [" << partition_seq.first
               << "] cyclic partition at dim=" << partition_seq.second.second.first
               << " factor=" << partition_seq.second.second.second << "\n";
    }
    for (auto partition_seq : tb.blockPartitionConfigs)
    {
        stream << "    Array: [" << partition_seq.second.first << "] in Function: [" << partition_seq.first
               << "] block partition at dim=" << partition_seq.second.second.first
               << " factor=" << partition_seq.second.second.second << "\n";
    }
    for (auto partition_seq : tb.completePartitionConfigs)
    {
        stream << "    Array: [" << partition_seq.second.first << "] in Function: [" << partition_seq.first
               << "] complete partition at dim=" << partition_seq.second.second << "\n";
    }
    stream << "\n\n";

    // loop_pipeline label=Loop_kernel_2mm_6 II=1
    // loop_unroll label=Loop_kernel_2mm_6 factor=8
    // array_partition variable=tmp dim=1 factor=8 scope=kernel_2mm cyclic
    // array_partition variable=D dim=1 factor=8 scope=kernel_2mm cyclic
    // array_partition variable=C dim=2 factor=8 scope=kernel_2mm cyclic

    for (auto unroll_pair : tb.loopUnrollConfigs)
    {
        stream << "loop_unroll label=" << unroll_pair.first << " factor=" << unroll_pair.second << "\n";
    }
    for (auto pipeline_pair : tb.loopPipelineConfigs)
    {
        stream << "loop_pipeline label=" << pipeline_pair.first << " II=" << pipeline_pair.second << "\n";
    }
    for (auto partition_seq : tb.cyclicPartitionConfigs)
    {
        stream << "array_partition variable=" << partition_seq.second.first << " scope=" << partition_seq.first
               << " dim=" << partition_seq.second.second.first << " factor=" << partition_seq.second.second.second
               << " cyclic\n";
    }
    for (auto partition_seq : tb.blockPartitionConfigs)
    {
        stream << "array_partition variable=" << partition_seq.second.first << " scope=" << partition_seq.first
               << " dim=" << partition_seq.second.second.first << " factor=" << partition_seq.second.second.second
               << " block\n";
    }

    stream << "\n\n";

    for (auto partition_seq : tb.cyclicPartitionConfigs)
    {
        stream << "#pragma HLS array_partition  variable=" << partition_seq.second.first
               << " dim=" << partition_seq.second.second.first << " factor=" << partition_seq.second.second.second
               << " cyclic\n";
    }

    for (auto partition_seq : tb.blockPartitionConfigs)
    {
        stream << "#pragma HLS array_partition  variable=" << partition_seq.second.first
               << " dim=" << partition_seq.second.second.first << " factor=" << partition_seq.second.second.second
               << " block\n";
    }

    for (auto partition_seq : tb.completePartitionConfigs)
    {
        stream << "#pragma HLS array_partition  variable=" << partition_seq.second.first
               << " dim=" << partition_seq.second.second << " complete\n";
    }

    return stream;
}

// int HI_WithDirectiveTimingResourceEvaluation::get_N_DSP(std::string opcode, int operand_bitwid ,
// int res_bitwidth, std::string period)
// {
//     if (checkInfoAvailability( opcode, operand_bitwid , res_bitwidth, period))
//         return BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period].DSP;
//     BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period] =
//     checkInfo_HigherFreq(opcode, operand_bitwid , res_bitwidth, period); return
//     BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period].DSP; llvm::errs() <<
//     "inquirying : " << opcode << " -- " << operand_bitwid << " -- " << res_bitwidth << " -- " <<
//     period << " \n"; assert(false && "no such information in the database\n");
// }

// int HI_WithDirectiveTimingResourceEvaluation::get_N_FF(std::string opcode, int operand_bitwid ,
// int res_bitwidth, std::string period)
// {
//     if (checkInfoAvailability( opcode, operand_bitwid , res_bitwidth, period))
//         return BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period].FF;
//     BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period] =
//     checkInfo_HigherFreq(opcode, operand_bitwid , res_bitwidth, period); return
//     BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period].FF; llvm::errs() <<
//     "inquirying : " << opcode << " -- " << operand_bitwid << " -- " << res_bitwidth << " -- " <<
//     period << " \n"; assert(false && "no such information in the database\n");
// }

// int HI_WithDirectiveTimingResourceEvaluation::get_N_LUT(std::string opcode, int operand_bitwid ,
// int res_bitwidth, std::string period)
// {
//     if (checkInfoAvailability( opcode, operand_bitwid , res_bitwidth, period))
//         return BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period].LUT;
//     BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period] =
//     checkInfo_HigherFreq(opcode, operand_bitwid , res_bitwidth, period); return
//     BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period].LUT; llvm::errs() <<
//     "inquirying : " << opcode << " -- " << operand_bitwid << " -- " << res_bitwidth << " -- " <<
//     period << " \n"; assert(false && "no such information in the database\n");
// }

// int HI_WithDirectiveTimingResourceEvaluation::get_N_Lat(std::string opcode, int operand_bitwid ,
// int res_bitwidth, std::string period)
// {
//     if (checkInfoAvailability( opcode, operand_bitwid , res_bitwidth, period))
//         return BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period].Lat;
//     BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period] =
//     checkInfo_HigherFreq(opcode, operand_bitwid , res_bitwidth, period); return
//     BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period].Lat; llvm::errs() <<
//     "inquirying : " << opcode << " -- " << operand_bitwid << " -- " << res_bitwidth << " -- " <<
//     period << " \n"; assert(false && "no such information in the database\n");
// }

// double HI_WithDirectiveTimingResourceEvaluation::get_N_Delay(std::string opcode, int
// operand_bitwid , int res_bitwidth, std::string period)
// {
//     if (checkInfoAvailability( opcode, operand_bitwid , res_bitwidth, period))
//         return BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period].delay;
//     BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period] =
//     checkInfo_HigherFreq(opcode, operand_bitwid , res_bitwidth, period); return
//     BiOp_Info_name2list_map[opcode][operand_bitwid][res_bitwidth][period].delay; llvm::errs() <<
//     "inquirying : " << opcode << " -- " << operand_bitwid << " -- " << res_bitwidth << " -- " <<
//     period << " \n"; assert(false && "no such information in the database\n");
// }
