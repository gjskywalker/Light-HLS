#include "HI_FloatingPointRTL.h"
#include "HI_FloatingPoint.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include <fstream>
#include <stdexcept>
#include <vector>

using namespace llvm;
using namespace light_hls::backend;

namespace {
std::string parameters(unsigned width)
{
    switch (width) {
    case 16: return "5,11";
    case 32: return "8,24";
    case 64: return "11,53";
    default: throw std::runtime_error("Unsupported HardFloat format width");
    }
}

void embed(std::ostream &out, const std::string &file, unsigned depth = 0)
{
    if (depth > 4)
        throw std::runtime_error("Recursive HardFloat include");
    std::ifstream input(std::string(LIGHT_HLS_HARDFLOAT_DIR) + "/" + file);
    if (!input)
        throw std::runtime_error("Missing vendored HardFloat source: " + file);
    std::string line;
    while (std::getline(input, line))
    {
        if (line.rfind("`include \"", 0) == 0)
        {
            std::string include = line.substr(10, line.find('"', 10) - 10);
            if (include == "HardFloat_specialize.vi")
                include = "RISCV/" + include;
            embed(out, include, depth + 1);
        }
        else
            out << line << '\n';
    }
}
}

bool emitFloatingOperator(std::ostream &out, const Instruction &instruction,
                          const std::string &n, const std::string &start,
                          const std::string &enable)
{
    if (!isFloatingOperator(instruction) || isa<SelectInst, FreezeInst, BitCastInst>(instruction))
        return false;
    const unsigned resultWidth = scalarBitWidth(instruction.getType());
    const auto *fpIntrinsic = dyn_cast<IntrinsicInst>(&instruction);
    const bool squareRoot = fpIntrinsic && fpIntrinsic->getIntrinsicID() == Intrinsic::sqrt;
    if (fpIntrinsic && (fpIntrinsic->getIntrinsicID() == Intrinsic::fabs ||
                        fpIntrinsic->getIntrinsicID() == Intrinsic::copysign))
    {
        out << "  assign " << n << "_out = {"
            << (fpIntrinsic->getIntrinsicID() == Intrinsic::fabs ? "1'b0" :
                n + "_in1[" + std::to_string(resultWidth-1) + "]")
            << ", " << n << "_in0[" << resultWidth-2 << ":0]};\n";
        return true;
    }
    if (instruction.getOpcode() == Instruction::FNeg)
    {
        out << "  assign " << n << "_out = {~" << n << "_in0[" << resultWidth-1
            << "], " << n << "_in0[" << resultWidth-2 << ":0]};\n";
        return true;
    }
    for (unsigned index = 0; index < instruction.getNumOperands(); ++index)
    {
        const Type *type = instruction.getOperand(index)->getType();
        if (!isSupportedFloat(type))
            continue;
        const unsigned width = scalarBitWidth(type);
        out << "  wire [" << width << ":0] " << n << "_rec" << index << ";\n"
            << "  fNToRecFN #(" << parameters(width) << ") " << n << "_decode" << index
            << " (" << n << "_in" << index << ", " << n << "_rec" << index << ");\n";
    }
    if (const auto *comparison = dyn_cast<FCmpInst>(&instruction))
    {
        const unsigned width = scalarBitWidth(instruction.getOperand(0)->getType());
        out << "  wire " << n << "_lt, " << n << "_eq, " << n << "_gt, " << n << "_un;\n"
            << "  compareRecFN #(" << parameters(width) << ") " << n << "_compare"
            << " (.a(" << n << "_rec0), .b(" << n << "_rec1), .signaling(1'b0),"
            << " .lt(" << n << "_lt), .eq(" << n << "_eq), .gt(" << n << "_gt),"
            << " .unordered(" << n << "_un), .exceptionFlags());\n";
        const std::vector<std::string> predicates = {
            "1'b0", n+"_eq", n+"_gt", "("+n+"_gt | "+n+"_eq)", n+"_lt",
            "("+n+"_lt | "+n+"_eq)", "("+n+"_lt | "+n+"_gt)", "~"+n+"_un",
            n+"_un", "("+n+"_un | "+n+"_eq)", "("+n+"_un | "+n+"_gt)",
            "~"+n+"_lt", "("+n+"_un | "+n+"_lt)", "~"+n+"_gt", "~"+n+"_eq", "1'b1"};
        out << "  assign " << n << "_out = " << predicates.at(comparison->getPredicate()) << ";\n";
        return true;
    }
    if (isa<FPToSIInst, FPToUIInst>(instruction))
    {
        const unsigned width = scalarBitWidth(instruction.getOperand(0)->getType());
        out << "  recFNToIN #(" << parameters(width) << "," << resultWidth << ") " << n << "_convert"
            << " (.control(1'b0), .in(" << n << "_rec0), .roundingMode(3'd1), .signedOut(1'b"
            << isa<FPToSIInst>(instruction) << "), .out(" << n << "_out), .intExceptionFlags());\n";
        return true;
    }
    out << "  wire [" << resultWidth << ":0] " << n << "_result;\n";
    if (isa<SIToFPInst, UIToFPInst>(instruction))
    {
        out << "  iNToRecFN #(" << scalarBitWidth(instruction.getOperand(0)->getType()) << ","
            << parameters(resultWidth) << ") " << n << "_convert"
            << " (.control(1'b0), .signedIn(1'b" << isa<SIToFPInst>(instruction) << "), .in("
            << n << "_in0), .roundingMode(3'd0), .out(" << n << "_result), .exceptionFlags());\n";
    }
    else if (isa<FPExtInst, FPTruncInst>(instruction))
    {
        out << "  recFNToRecFN #(" << parameters(scalarBitWidth(instruction.getOperand(0)->getType()))
            << "," << parameters(resultWidth) << ") " << n << "_convert"
            << " (.control(1'b0), .in(" << n << "_rec0), .roundingMode(3'd0), .out("
            << n << "_result), .exceptionFlags());\n";
    }
    else if (isIterativeFloating(instruction))
    {
        const unsigned fraction = resultWidth == 16 ? 10 : resultWidth == 64 ? 52 : 23;
        const unsigned exponent = resultWidth - fraction - 1;
        if (squareRoot)
        {
            out << "  wire " << n << "_special = " << n << "_in0[" << resultWidth-1
                << "] || (&" << n << "_in0[" << resultWidth-2 << ":" << fraction
                << "]) || !(|" << n << "_in0[" << resultWidth-2 << ":0]);\n"
                << "  wire " << n << "_nan = (" << n << "_in0[" << resultWidth-1
                << "] && (|" << n << "_in0[" << resultWidth-2 << ":0])) || ((&"
                << n << "_in0[" << resultWidth-2 << ":" << fraction << "]) && (|"
                << n << "_in0[" << fraction-1 << ":0]));\n"
                << "  wire [" << resultWidth-1 << ":0] " << n << "_special_bits = " << n << "_nan ? "
                << (resultWidth == 16 ? "16'h7e00" : resultWidth == 64 ? "64'h7ff8000000000000" : "32'h7fc00000")
                << " : " << n << "_in0;\n";
        }
        else
        out << "  wire " << n << "_special = (&" << n << "_in0[" << resultWidth-2 << ":" << fraction
            << "]) || (&" << n << "_in1[" << resultWidth-2 << ":" << fraction
            << "]) || !(|" << n << "_in0[" << resultWidth-2 << ":0]) || !(|" << n << "_in1[" << resultWidth-2 << ":0]);\n"
            << "  wire " << n << "_nan = ((&" << n << "_in0[" << resultWidth-2 << ":" << fraction
            << "]) && (|" << n << "_in0[" << fraction-1 << ":0])) || ((&" << n << "_in1["
            << resultWidth-2 << ":" << fraction << "]) && (|" << n << "_in1[" << fraction-1 << ":0])) ||"
            << " ((&" << n << "_in0[" << resultWidth-2 << ":" << fraction << "]) && (&" << n << "_in1["
            << resultWidth-2 << ":" << fraction << "])) || (!(|" << n << "_in0[" << resultWidth-2
            << ":0]) && !(|" << n << "_in1[" << resultWidth-2 << ":0]));\n"
            << "  wire [" << resultWidth-1 << ":0] " << n << "_special_bits = " << n << "_nan ? "
            << (resultWidth == 16 ? "16'h7e00" : resultWidth == 64 ? "64'h7ff8000000000000" : "32'h7fc00000") << " : {"
            << n << "_in0[" << resultWidth-1 << "] ^ " << n << "_in1[" << resultWidth-1 << "], {"
            << exponent << "{(&" << n << "_in0[" << resultWidth-2 << ":" << fraction
            << "]) || !(|" << n << "_in1[" << resultWidth-2 << ":0])}}, " << fraction << "'d0};\n";
        out << "  wire [" << resultWidth << ":0] " << n << "_special_rec;\n"
            << "  fNToRecFN #(" << parameters(resultWidth) << ") " << n << "_special_decode ("
            << n << "_special_bits, " << n << "_special_rec);\n";
        out << "  wire " << n << "_valid;\n  wire [" << resultWidth << ":0] " << n << "_quotient;\n"
            << "  reg [" << resultWidth << ":0] " << n << "_held;\n"
            << "  divSqrtRecFN_small #(" << parameters(resultWidth) << ",0) " << n << "_divide"
            << " (.nReset(~ap_rst), .clock(ap_clk), .clockEnable(" << enable << "), .control(1'b0), .inReady(), .inValid(!" << n << "_special && " << start
            << "), .sqrtOp(1'b" << squareRoot << "), .a(" << n << "_rec0), .b("
            << (squareRoot ? std::to_string(resultWidth+1)+"'d0" : n+"_rec1") << "), .roundingMode(3'd0),"
            << " .outValid(" << n << "_valid), .sqrtOpOut(), .out(" << n << "_quotient), .exceptionFlags());\n"
            << "  always @(posedge ap_clk) begin\n    if (ap_rst) " << n << "_held <= 0;\n"
            << "    else if (" << enable << " && " << start << " && " << n << "_special) " << n << "_held <= " << n << "_special_rec;\n"
            << "    else if (" << enable << " && " << n << "_valid) " << n << "_held <= " << n << "_quotient;\n  end\n"
            << "  assign " << n << "_result = " << n << "_held;\n";
    }
    else if (const auto *intrinsic = dyn_cast<IntrinsicInst>(&instruction))
    {
        if (intrinsic->getIntrinsicID() != Intrinsic::fma)
            throw std::runtime_error("Unimplemented floating-point intrinsic reached RTL emission");
        out << "  mulAddRecFN #(" << parameters(resultWidth) << ") " << n << "_fma"
            << " (.control(1'b0), .op(2'b00), .a(" << n << "_rec0), .b(" << n << "_rec1),"
            << " .c(" << n << "_rec2), .roundingMode(3'd0), .out(" << n << "_result), .exceptionFlags());\n";
    }
    else
    {
        if (instruction.getOpcode() != Instruction::FAdd &&
            instruction.getOpcode() != Instruction::FSub &&
            instruction.getOpcode() != Instruction::FMul)
            throw std::runtime_error("Unimplemented floating opcode reached RTL emission");
        const bool multiply = instruction.getOpcode() == Instruction::FMul;
        out << "  " << (multiply ? "mulRecFN" : "addRecFN") << " #(" << parameters(resultWidth)
            << ") " << n << "_arithmetic (.control(1'b0), ";
        if (!multiply)
            out << ".subOp(1'b" << (instruction.getOpcode() == Instruction::FSub) << "), ";
        out << ".a(" << n << "_rec0), .b(" << n << "_rec1), .roundingMode(3'd0), .out("
            << n << "_result), .exceptionFlags());\n";
    }
    out << "  recFNToFN #(" << parameters(resultWidth) << ") " << n << "_encode ("
        << n << "_result, " << n << "_out);\n";
    return true;
}

void emitHardFloatLibrary(std::ostream &out)
{
    out << "\n`ifndef LIGHT_HLS_HARDFLOAT_LIBRARY\n`define LIGHT_HLS_HARDFLOAT_LIBRARY\n";
    out << "\n// Berkeley HardFloat Release 1, BSD-3-Clause; original notices follow.\n";
    for (const char *file : {"HardFloat_primitives.v", "HardFloat_rawFN.v",
            "RISCV/HardFloat_specialize.v", "isSigNaNRecFN.v", "fNToRecFN.v", "recFNToFN.v",
            "addRecFN.v", "mulRecFN.v", "mulAddRecFN.v", "compareRecFN.v", "recFNToIN.v", "iNToRecFN.v",
            "recFNToRecFN.v", "divSqrtRecFN_small.v"})
        embed(out, file);
    out << "\n`endif\n";
}
