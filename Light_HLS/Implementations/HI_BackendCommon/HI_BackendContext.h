#ifndef HI_BACKEND_CONTEXT_H
#define HI_BACKEND_CONTEXT_H

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace llvm
{
class AllocaInst;
class CallBase;
class ConstantInt;
class PHINode;
class SCEV;
class Value;
}

namespace light_hls
{
namespace backend
{

enum class BackendFeature
{
    SequentialControlFlow,
    IntegerArithmetic,
    LoopPipeline,
    ArrayPartition,
    Dataflow,
    FloatingPoint,
    MemoryInterface,
    StreamInterface
};

enum class ValidationStage
{
    PreBackend,
    PostRTL
};

enum class DiagnosticSeverity
{
    Warning,
    Error
};

struct BackendDiagnostic
{
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string code;
    std::string message;
    std::string location;
};

struct BackendCapabilities
{
    bool sequentialControlFlow = true;
    bool integerArithmetic = true;
    bool loopPipeline = true;
    bool arrayPartition = true;
    bool dataflow = false;
    bool floatingPoint = false;
    bool memoryInterface = true;
    bool streamInterface = false;
};

enum class PartitionKind
{
    None,
    Cyclic,
    Block,
    Complete
};

enum class MemoryBindingType
{
    Automatic,
    RAM1P,
    RAM2P
};

enum class MemoryImplementation
{
    Automatic,
    BlockRAM,
    UltraRAM,
    DistributedRAM
};

struct ArrayPartitionDirective
{
    std::string scope;
    std::string memory;
    PartitionKind kind = PartitionKind::None;
    unsigned dimension = 0;
    unsigned factor = 0;
};

struct StorageBindingDirective
{
    std::string scope;
    std::string memory;
    MemoryBindingType type = MemoryBindingType::Automatic;
    MemoryImplementation implementation = MemoryImplementation::Automatic;
    unsigned readLatency = 1;
};

struct StreamDirective
{
    std::string scope;
    std::string variable;
    std::string type = "fifo";
    unsigned depth = 2;
    unsigned bitWidth = 0;
    unsigned instances = 1;
    unsigned initialTokens = 0;
};

struct RequiredFeatures
{
    bool sequentialControlFlow = false;
    bool integerArithmetic = false;
    bool loopPipeline = false;
    bool arrayPartition = false;
    bool dataflow = false;
    bool floatingPoint = false;
    bool memoryInterface = false;
    bool streamInterface = false;
};

struct PragmaConfiguration
{
    std::vector<std::pair<std::string, int>> unroll;
    std::vector<std::pair<std::string, int>> pipeline;
    unsigned arrayPartitionCount = 0;
    unsigned dataflowCount = 0;
    unsigned streamCount = 0;
};

struct BackendConfiguration
{
    std::string design;
    std::string topFunction;
    std::string rtlModuleName;
    bool rtlClockEnable = false;
    std::string sourcePath;
    std::string sourceHash;
    std::string board;
    std::string outputDirectory;
    double clockPeriodNs = 10.0;
    PragmaConfiguration pragmas;
    std::map<std::string, unsigned long long> frontendMemoryDepths;
    std::map<std::string, std::vector<unsigned long long>>
        frontendMemoryDimensions;
    std::vector<ArrayPartitionDirective> arrayPartitions;
    std::vector<StorageBindingDirective> storageBindings;
    std::vector<StreamDirective> streams;
    // Internal task argument indices confirmed as FIFO endpoints by DATAFLOW.
    // This is not a user option and does not enable generic volatile memory.
    std::set<unsigned> fifoArguments;
    // Internal FWFT endpoints whose consumers sample data on the accepted pop edge.
    std::set<unsigned> fifoReadOnRequestArguments;
    // Internal PIPO payloads are observed only on accepted write edges.
    std::set<unsigned> internalPIPOWriteArguments;
    std::map<std::string, unsigned long long> configuredMemoryDepths;
    std::map<std::string, unsigned> configuredMemoryPorts;
    std::map<std::string, unsigned> configuredMemoryReadLatencies;
    std::map<std::string, long long> dataflowTaskLatencies;
    std::map<std::string, unsigned> dataflowTaskInitiationIntervals;
    std::map<std::string, std::string> irLoopLabels;
    std::map<std::string, int> irLoopTripCounts;
    bool runSynthesis = false;
    std::string synthesisTool = "vivado";
    std::string synthesisExecutable = "vivado";
    std::string synthesisPart;
    unsigned synthesisTimeoutSeconds = 900;
    std::vector<std::string> passSequence;
};

struct PreBackendEstimate
{
    bool valid = false;
    long long latencyCycles = -1;
    int initiationInterval = 0;
    int dsp = -1;
    int ff = -1;
    int lut = -1;
    int bram = -1;
};

struct InstructionSchedule
{
    const llvm::Instruction *instruction = nullptr;
    std::string name;
    std::string opcode;
    std::string block;
    unsigned resultWidth = 0;
    int startCycle = 0;
    int latency = 1;
};

struct PipelinePhiInfo
{
    const llvm::PHINode *phi = nullptr;
    const llvm::Value *initialValue = nullptr;
    const llvm::Value *recurrenceValue = nullptr;
    bool affine = false;
    long long affineStep = 0;
};

enum class DependenceKind
{
    ScalarRecurrence,
    MemoryRAW,
    MemoryWAR,
    MemoryWAW,
    FIFOOrder
};

enum class MemoryEventKind
{
    ReadRequest,
    WriteRequest
};

struct ScalarRecurrenceDependence
{
    const llvm::PHINode *phi = nullptr;
    const llvm::Instruction *source = nullptr;
    std::vector<const llvm::Instruction *> sinks;
    unsigned distance = 1;
    bool exact = false;
    std::string reason;
};

struct AffineMemoryAccessPattern
{
    const llvm::Instruction *instruction = nullptr;
    std::string memory;
    bool isWrite = false;
    bool affine = false;
    long long coefficient = 0;
    long long constant = 0;
    long long minimum = 0;
    long long maximum = 0;
    unsigned order = 0;
    std::map<const llvm::SCEV *, long long> invariantTerms;
    bool bankMappingExact = false;
    std::vector<unsigned> possibleBanks;
    std::string reason;
};

struct MemoryDependenceInfo
{
    const llvm::Instruction *source = nullptr;
    const llvm::Instruction *sink = nullptr;
    std::string memory;
    DependenceKind kind = DependenceKind::MemoryRAW;
    MemoryEventKind sourceEvent = MemoryEventKind::ReadRequest;
    MemoryEventKind sinkEvent = MemoryEventKind::ReadRequest;
    int sourceEventOffset = 0;
    int sinkEventOffset = 0;
    unsigned minimumSeparation = 1;
    unsigned distance = 0;
    bool loopCarried = false;
    bool exists = true;
    bool exact = false;
    std::string reason;
};

struct LoopPipelineInfo
{
    bool valid = false;
    const llvm::BasicBlock *header = nullptr;
    const llvm::BasicBlock *preheader = nullptr;
    const llvm::BasicBlock *latch = nullptr;
    const llvm::BasicBlock *exit = nullptr;
    std::string irLoopName;
    std::string label;
    int requestedII = 0;
    int achievedII = 0;
    int recurrenceMII = 1;
    int memoryMII = 1;
    int memoryDependenceMII = 1;
    int resourceMII = 1;
    unsigned tripCount = 0;
    unsigned sourceTripCount = 0;
    unsigned flattenedLoopCount = 1;
    std::string flatteningReason;
    unsigned pipelineLatency = 0;
    unsigned stageCount = 0;
    unsigned contextSlots = 0;
    bool conservativeMemoryDependence = false;
    std::string limitingFactor;
    std::vector<const llvm::BasicBlock *> blocks;
    std::vector<PipelinePhiInfo> phis;
    bool dependenceAnalysisValid = false;
    std::vector<ScalarRecurrenceDependence> scalarRecurrences;
    std::vector<AffineMemoryAccessPattern> memoryAccessPatterns;
    std::vector<MemoryDependenceInfo> memoryDependences;
    std::set<const llvm::Value *> contextValues;
    std::set<const llvm::Value *> derivedValues;
    std::set<const llvm::Value *> liveOutValues;
    std::set<const llvm::Instruction *> forwardedLoads;
    std::map<const llvm::Value *, unsigned> valueStorageSlots;
    std::map<const llvm::Value *, std::pair<int, int>> valueLiveIntervals;
    std::set<const llvm::Value *> retirementOnlyValues;
    bool stageIndexedController = false;
    bool lastTokenController = false;
    int iterationTagLastStage = -1;
    const llvm::Instruction *forwardedReturnValue = nullptr;
    bool initializedAtEntry = false;
    std::map<const llvm::Value *, const llvm::Value *> storageAliases;
    std::map<const llvm::Instruction *, int> operationStartCycle;
    std::map<std::string, unsigned> operationStage;
    std::map<std::string, int> iterationOffset;
};

struct BlockSchedule
{
    const llvm::BasicBlock *block = nullptr;
    std::string name;
    unsigned firstState = 0;
    unsigned stateCount = 0;
    long long executionLatency = 0;
    bool pipelined = false;
    std::vector<InstructionSchedule> instructions;
};

struct ScheduleResult
{
    bool valid = false;
    bool hasControlFlowCycle = false;
    long long latencyCycles = -1;
    int achievedII = 0;
    unsigned stateCount = 0;
    unsigned memoryPortConflicts = 0;
    unsigned memoryPortSerializationCycles = 0;
    unsigned memoryOrderingCycles = 0;
    std::vector<BlockSchedule> blocks;
    std::vector<LoopPipelineInfo> pipelines;
    std::map<const llvm::Instruction *, InstructionSchedule *> instructionMap;
};

struct OperatorInstance
{
    std::string name;
    std::string opcode;
    unsigned bitWidth = 0;
    std::vector<unsigned> operandWidths;
    std::vector<std::string> operations;
    // Captured arguments and constants shared by every operation on this FU.
    std::map<unsigned, const llvm::Value *> invariantOperands;
};

struct OperationBinding
{
    const llvm::Instruction *instruction = nullptr;
    std::string operation;
    std::string instance;
};

struct MemoryAccessBinding
{
    const llvm::Instruction *instruction = nullptr;
    std::string operation;
    std::string memory;
    unsigned bank = 0;
    bool bankDynamic = false;
    std::vector<unsigned> possibleBanks;
    unsigned port = 0;
    // Banks whose local RAM write payload has this sole static producer.
    std::vector<unsigned> directWriteDataBanks;
};

struct BindingResult
{
    bool valid = false;
    unsigned registerCount = 0;
    unsigned long long registerBits = 0;
    unsigned stateRegisterBits = 0;
    unsigned muxCount = 0;
    unsigned muxInputCount = 0;
    unsigned long long ordinalMuxBitCost = 0;
    unsigned long long selectedMuxBitCost = 0;
    unsigned affinityReboundGroups = 0;
    std::vector<OperatorInstance> operators;
    std::vector<OperationBinding> operations;
    std::vector<MemoryAccessBinding> memoryAccesses;
};

struct RTLSynthesisResult
{
    bool requested = false;
    bool attempted = false;
    bool valid = false;
    std::string tool;
    std::string toolVersion;
    std::string part;
    int cpuAffinity = -1;
    int exitCode = -1;
    double runtimeSeconds = 0.0;
    long long lut = -1;
    long long ff = -1;
    long long dsp = -1;
    double bramTiles = -1.0;
    double worstSlackNs = 0.0;
    bool hasTiming = false;
};

enum class RTLInterfaceProtocol
{
    Scalar,
    APControlHS,
    APMemory,
    AXIMaster,
    AXILite,
    FIFO,
    BRAM
};

enum class MemoryDepthSource
{
    Unknown,
    Frontend,
    Configuration,
    Allocation,
    Inferred
};

enum class MemoryStorageKind
{
    ExternalAPMemory,
    LocalRAM
};

enum class MemoryAccessMode
{
    Unused,
    ReadOnly,
    WriteOnly,
    ReadWrite
};

struct MemoryPortUsage
{
    bool hasRead = false;
    bool hasWrite = false;
};

struct MemoryPartitionDimension
{
    unsigned dimension = 0;
    PartitionKind kind = PartitionKind::None;
    unsigned long long extent = 0;
    unsigned factor = 1;
    unsigned long long localExtent = 0;
};

struct MemoryObject
{
    const llvm::Value *base = nullptr;
    const llvm::Argument *argument = nullptr;
    const llvm::AllocaInst *allocation = nullptr;
    std::string name;
    std::string portName;
    unsigned elementWidth = 0;
    unsigned addressWidth = 64;
    unsigned long long depth = 0;
    unsigned long long inferredMinimumDepth = 0;
    MemoryDepthSource depthSource = MemoryDepthSource::Unknown;
    bool depthExact = false;
    unsigned readLatency = 1;
    unsigned ports = 1;
    bool fifoReadOnRequest = false;
    MemoryStorageKind storage = MemoryStorageKind::ExternalAPMemory;
    MemoryBindingType bindingType = MemoryBindingType::Automatic;
    MemoryImplementation implementation = MemoryImplementation::Automatic;
    bool storageBindingExplicit = false;
    PartitionKind partition = PartitionKind::None;
    unsigned banks = 1;
    unsigned long long bankDepth = 0;
    unsigned bankAddressWidth = 64;
    std::vector<unsigned long long> dimensions;
    std::vector<MemoryPartitionDimension> partitionDimensions;
    MemoryAccessMode accessMode = MemoryAccessMode::Unused;
    std::vector<MemoryPortUsage> portUsage;
    std::vector<std::vector<MemoryPortUsage>> bankPortUsage;

    // Fully scalarized external banks are input wires, not synchronous RAMs.
    bool hasCombinationalScalarReads() const
    {
        return storage == MemoryStorageKind::ExternalAPMemory && banks > 1 &&
               bankDepth == 1 && banks == depth && readLatency == 0;
    }
};

struct MemoryAccessInfo
{
    const llvm::Instruction *instruction = nullptr;
    const llvm::Value *memoryBase = nullptr;
    const llvm::Value *pointerOperand = nullptr;
    const llvm::Value *writeValue = nullptr;
    std::string memory;
    unsigned dataWidth = 0;
    bool isWrite = false;
    bool isFIFO = false;
    bool bankMappingExact = false;
    std::vector<unsigned> possibleBanks;
    std::string bankAnalysisSource;
    std::string bankAnalysisReason;
    unsigned bankAnalysisIterations = 0;
    // Exact logical-iteration trace; an empty trace never proves disjointness.
    std::vector<unsigned> iterationBanks;
};

struct DataflowTask
{
    std::string name;
    std::string function;
    const llvm::Function *functionValue = nullptr;
    const llvm::CallBase *call = nullptr;
    long long latencyCycles = -1;
    std::string latencySource;
    bool latencyExact = false;
    unsigned initiationInterval = 0;
    unsigned pipelineInitiationInterval = 0;
    unsigned pipelineCount = 0;
};

struct DataflowChannel
{
    std::string name;
    std::string producer;
    std::string consumer;
    unsigned width = 0;
    unsigned depth = 0;
    unsigned instances = 1;
    unsigned initialTokens = 0;
    std::string type = "fifo";
    const llvm::Value *base = nullptr;
    bool rateKnown = false;
    bool rateBoundsKnown = false;
    bool runtimeRateCheckRequired = false;
    unsigned long long productionRate = 0;
    unsigned long long consumptionRate = 0;
    unsigned long long minimumProductionRate = 0;
    unsigned long long maximumProductionRate = 0;
    unsigned long long minimumConsumptionRate = 0;
    unsigned long long maximumConsumptionRate = 0;
    long long producerFirstAccessCycle = -1;
    long long consumerFirstAccessCycle = -1;
    unsigned producerAccessInterval = 0;
    unsigned consumerAccessInterval = 0;
    bool producerAccessTimingExact = false;
    bool consumerAccessTimingExact = false;
    unsigned minimumDepth = 0;
    unsigned bufferCount = 1;
    bool capacitySafe = false;
    bool consumerReadOnRequest = false;
    PartitionKind partition = PartitionKind::None;
    unsigned banks = 1;
    unsigned long long bankDepth = 0;
    bool broadcast = false;
};

struct DataflowGraph
{
    bool valid = false;
    bool performanceValid = false;
    bool ratesBalanced = false;
    bool deadlockFree = false;
    bool cyclic = false;
    bool zeroTokenCycle = false;
    unsigned graphInitiationInterval = 0;
    std::string graphIISource;
    std::string executionModel;
    unsigned maximumOutstandingTransactions = 1;
    unsigned minimumAcceptanceInterval = 0;
    long long measuredAcceptanceInterval = -1;
    long long firstTransactionLatencyLowerBound = -1;
    long long firstTransactionLatencyUpperBound = -1;
    bool firstTransactionLatencyExact = false;
    std::string firstTransactionLatencySource;
    unsigned pipoHandoffCycles = 1;
    unsigned retirementCycles = 1;
    std::vector<std::string> topologicalOrder;
    std::vector<DataflowTask> tasks;
    std::vector<DataflowChannel> channels;
};

struct DataflowTaskPortAccess
{
    unsigned argumentIndex = 0;
    std::vector<const llvm::Instruction *> reads;
    std::vector<const llvm::Instruction *> writes;
    long long firstReadCycle = -1;
    long long firstWriteCycle = -1;
    unsigned readInterval = 0;
    unsigned writeInterval = 0;
    bool readTimingExact = false;
    bool writeTimingExact = false;
};

struct DataflowTaskImplementation
{
    std::string task;
    std::string moduleName;
    std::string verilog;
    std::vector<MemoryObject> memories;
    std::vector<DataflowTaskPortAccess> portAccesses;
    long long latencyCycles = -1;
    std::string latencySource;
    bool latencyExact = false;
    unsigned initiationInterval = 0;
    unsigned pipelineInitiationInterval = 0;
    unsigned pipelineCount = 0;
};

struct OperatorImplementation
{
    std::string operation;
    std::string implementation;
    unsigned latency = 1;
    unsigned initiationInterval = 1;
};

struct RTLDesign
{
    bool valid = false;
    std::string moduleName;
    std::string verilog;
    std::vector<RTLInterfaceProtocol> interfaces;
    std::vector<MemoryObject> memories;
    std::vector<MemoryAccessBinding> memoryBindings;
    DataflowGraph dataflow;
    std::vector<LoopPipelineInfo> pipelines;
    std::vector<OperatorImplementation> operatorImplementations;
};

struct OrdinaryReadPhiBypass
{
    const llvm::BasicBlock *predecessor = nullptr;
    const llvm::PHINode *phi = nullptr;
    const llvm::Instruction *load = nullptr;
};

struct HLSBackendContext
{
    BackendConfiguration configuration;
    BackendCapabilities capabilities;
    RequiredFeatures requiredFeatures;
    llvm::Module *module = nullptr;
    llvm::Function *top = nullptr;
    PreBackendEstimate estimate;
    ScheduleResult schedule;
    BindingResult binding;
    RTLSynthesisResult synthesis;
    RTLDesign rtl;
    std::vector<BackendDiagnostic> diagnostics;
    std::map<const llvm::Instruction *, int> latencyModel;
    std::map<const llvm::Instruction *, double> delayModel;
    std::set<const llvm::Instruction *> combinationalValues;
    // Constant-step ordinary-loop updates sampled only at their unique latch edge.
    std::map<const llvm::Instruction *, const llvm::PHINode *> ordinaryFeedbackValues;
    // Sample the bound operator output at the self-edge; retain its live-out register.
    std::map<const llvm::PHINode *, const llvm::Instruction *> ordinaryBackedgeBypasses;
    // A self-edge result and its PHI share storage when the old PHI is block-local.
    std::map<const llvm::Instruction *, const llvm::PHINode *> ordinaryRegisterAliases;
    // These old PHIs remain held on loop exit for combinational live-out expressions.
    std::set<const llvm::PHINode *> ordinaryLiveOutExpressionRoots;
    // Timed combinational return blocks completed on their incoming branch edge.
    std::set<const llvm::BasicBlock *> ordinaryReturnExits;
    const llvm::BasicBlock *ordinaryEntrySuccessor = nullptr;
    // Capture a memory response and successor PHI together; retain the load register.
    std::vector<OrdinaryReadPhiBypass> ordinaryReadPhiBypasses;
    std::set<const llvm::Instruction *> forwardedLoads;
    std::map<const llvm::Value *, std::string> valueNames;
    std::map<const llvm::Value *, const llvm::ConstantInt *> constantValues;
    std::vector<MemoryObject> memoryObjects;
    std::map<const llvm::Value *, const llvm::Value *> addressProvenance;
    std::map<const llvm::Instruction *, MemoryAccessInfo> memoryAccesses;
    std::vector<LoopPipelineInfo> pipelineRegions;
    std::vector<DataflowTaskImplementation> dataflowImplementations;

    void addError(const std::string &code, const std::string &message,
                  const std::string &location = "");
    void addWarning(const std::string &code, const std::string &message,
                    const std::string &location = "");
    bool hasErrors() const;
};

using HLSBackendContextPtr = std::shared_ptr<HLSBackendContext>;

HLSBackendContextPtr createBackendContext(llvm::Module &module,
                                          const BackendConfiguration &configuration);

std::string sanitizeVerilogIdentifier(const std::string &name,
                                      const std::string &fallback);
std::string sanitizeVerilogPortIdentifier(const std::string &name,
                                          const std::string &fallback);
std::string memoryDepthSourceName(MemoryDepthSource source);
std::string memoryAccessModeName(MemoryAccessMode mode);
std::string memoryBindingTypeName(MemoryBindingType type);
std::string memoryImplementationName(MemoryImplementation implementation);
std::string partitionKindName(PartitionKind kind);
std::string dependenceKindName(DependenceKind kind);
std::string memoryEventKindName(MemoryEventKind kind);
unsigned partitionBankForAddress(const MemoryObject &memory,
                                 unsigned long long address);
unsigned long long partitionLocalAddressForAddress(
    const MemoryObject &memory, unsigned long long address);
std::string stableFileHash(const std::string &path);
bool emitBackendArtifacts(const HLSBackendContext &context,
                          std::string &errorMessage);
// Delay of a pure entry expression evaluated from live ports on start acceptance.
double ordinaryEntryValueDelay(const HLSBackendContext &context, const llvm::Value *value);
// Tasks whose completion is not implied by a later task's accepted channel data.
std::set<std::string> dataflowCompletionTasks(const DataflowGraph &graph);
// Request-edge collision, including overlaps between different iterations.
bool memoryRequestsConflict(const MemoryAccessInfo &left, int leftStart,
                            const MemoryAccessInfo &right, int rightStart,
                            unsigned initiationInterval);
// Only proven leading zeroes may be removed from an unsigned multiplier input.
unsigned unsignedArithmeticWidth(const llvm::Value *value, const llvm::DataLayout &layout);

} // namespace backend
} // namespace light_hls

#endif
