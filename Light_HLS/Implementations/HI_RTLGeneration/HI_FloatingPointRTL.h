#ifndef HI_FLOATING_POINT_RTL_H
#define HI_FLOATING_POINT_RTL_H
#include "llvm/IR/Instruction.h"
#include <ostream>
#include <string>

bool emitFloatingOperator(std::ostream &out, const llvm::Instruction &instruction,
                          const std::string &name, const std::string &start,
                          const std::string &enable = "1'b1");
void emitHardFloatLibrary(std::ostream &out);
#endif
