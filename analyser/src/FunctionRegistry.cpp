#include "scope/analyser/FunctionRegistry.h"

namespace scope::analyser {

FunctionRegistry& FunctionRegistry::instance() {
    static FunctionRegistry reg;
    return reg;
}

void FunctionRegistry::registerFunction(FunctionDescriptor desc) {
    list_.push_back(std::move(desc));
}

}  // namespace scope::analyser
