//===- ConvertLaunchFuncToVulkanCalls.cpp - MLIR Vulkan conversion passes -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass to convert vulkan launch call into a sequence of
// Vulkan runtime calls. The Vulkan runtime API surface is huge so currently we
// don't expose separate external functions in IR for each of them, instead we
// expose a few external functions to wrapper libraries which manages Vulkan
// runtime.
//
//===----------------------------------------------------------------------===//

#include "mlir/Conversion/GPUToVulkan/ConvertGPUToVulkanPass.h"
#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Module.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FormatVariadic.h"

using namespace mlir;

static constexpr const char *kBindMemRef1DFloat = "bindMemRef1DFloat";
static constexpr const char *kBindMemRef2DFloat = "bindMemRef2DFloat";
static constexpr const char *kCInterfaceVulkanLaunch =
    "_mlir_ciface_vulkanLaunch";
static constexpr const char *kDeinitVulkan = "deinitVulkan";
static constexpr const char *kRunOnVulkan = "runOnVulkan";
static constexpr const char *kInitVulkan = "initVulkan";
static constexpr const char *kSetBinaryShader = "setBinaryShader";
static constexpr const char *kSetEntryPoint = "setEntryPoint";
static constexpr const char *kSetNumWorkGroups = "setNumWorkGroups";
static constexpr const char *kSPIRVBinary = "SPIRV_BIN";
static constexpr const char *kSPIRVBlobAttrName = "spirv_blob";
static constexpr const char *kSPIRVEntryPointAttrName = "spirv_entry_point";
static constexpr const char *kVulkanLaunch = "vulkanLaunch";

namespace {

/// A pass to convert vulkan launch call op into a sequence of Vulkan
/// runtime calls in the following order:
///
/// * initVulkan           -- initializes vulkan runtime
/// * bindMemRef           -- binds memref
/// * setBinaryShader      -- sets the binary shader data
/// * setEntryPoint        -- sets the entry point name
/// * setNumWorkGroups     -- sets the number of a local workgroups
/// * runOnVulkan          -- runs vulkan runtime
/// * deinitVulkan         -- deinitializes vulkan runtime
///
class VulkanLaunchFuncToVulkanCallsPass
    : public ModulePass<VulkanLaunchFuncToVulkanCallsPass> {
private:
  LLVM::LLVMDialect *getLLVMDialect() { return llvmDialect; }

  llvm::LLVMContext &getLLVMContext() {
    return getLLVMDialect()->getLLVMContext();
  }

  void initializeCachedTypes() {
    llvmDialect = getContext().getRegisteredDialect<LLVM::LLVMDialect>();
    llvmFloatType = LLVM::LLVMType::getFloatTy(llvmDialect);
    llvmVoidType = LLVM::LLVMType::getVoidTy(llvmDialect);
    llvmPointerType = LLVM::LLVMType::getInt8PtrTy(llvmDialect);
    llvmInt32Type = LLVM::LLVMType::getInt32Ty(llvmDialect);
    llvmInt64Type = LLVM::LLVMType::getInt64Ty(llvmDialect);
    initializeMemRefTypes();
  }

  void initializeMemRefTypes() {
    // According to the MLIR doc memref argument is converted into a
    // pointer-to-struct argument of type:
    // template <typename Elem, size_t Rank>
    // struct {
    //   Elem *allocated;
    //   Elem *aligned;
    //   int64_t offset;
    //   int64_t sizes[Rank]; // omitted when rank == 0
    //   int64_t strides[Rank]; // omitted when rank == 0
    // };
    auto llvmPtrToFloatType = getFloatType().getPointerTo();
    auto llvmArrayOneElementSizeType =
        LLVM::LLVMType::getArrayTy(getInt64Type(), 1);
    auto llvmArrayTwoElementSizeType =
        LLVM::LLVMType::getArrayTy(getInt64Type(), 2);

    // Create a type `!llvm<"{ float*, float*, i64, [1 x i64], [1 x i64]}">`.
    llvmMemRef1DFloat = LLVM::LLVMType::getStructTy(
        llvmDialect,
        {llvmPtrToFloatType, llvmPtrToFloatType, getInt64Type(),
         llvmArrayOneElementSizeType, llvmArrayOneElementSizeType});

    // Create a type `!llvm<"{ float*, float*, i64, [2 x i64], [2 x i64]}">`.
    llvmMemRef2DFloat = LLVM::LLVMType::getStructTy(
        llvmDialect,
        {llvmPtrToFloatType, llvmPtrToFloatType, getInt64Type(),
         llvmArrayTwoElementSizeType, llvmArrayTwoElementSizeType});
  }

  LLVM::LLVMType getFloatType() { return llvmFloatType; }
  LLVM::LLVMType getVoidType() { return llvmVoidType; }
  LLVM::LLVMType getPointerType() { return llvmPointerType; }
  LLVM::LLVMType getInt32Type() { return llvmInt32Type; }
  LLVM::LLVMType getInt64Type() { return llvmInt64Type; }
  LLVM::LLVMType getMemRef1DFloat() { return llvmMemRef1DFloat; }
  LLVM::LLVMType getMemRef2DFloat() { return llvmMemRef2DFloat; }

  /// Creates a LLVM global for the given `name`.
  Value createEntryPointNameConstant(StringRef name, Location loc,
                                     OpBuilder &builder);

  /// Declares all needed runtime functions.
  void declareVulkanFunctions(Location loc);

  /// Checks whether the given LLVM::CallOp is a vulkan launch call op.
  bool isVulkanLaunchCallOp(LLVM::CallOp callOp) {
    return (callOp.callee() && callOp.callee().getValue() == kVulkanLaunch &&
            callOp.getNumOperands() >= gpu::LaunchOp::kNumConfigOperands);
  }

  /// Checks whether the given LLVM::CallOp is a "ci_face" vulkan launch call
  /// op.
  bool isCInterfaceVulkanLaunchCallOp(LLVM::CallOp callOp) {
    return (callOp.callee() &&
            callOp.callee().getValue() == kCInterfaceVulkanLaunch &&
            callOp.getNumOperands() >= gpu::LaunchOp::kNumConfigOperands);
  }

  /// Translates the given `vulkanLaunchCallOp` to the sequence of Vulkan
  /// runtime calls.
  void translateVulkanLaunchCall(LLVM::CallOp vulkanLaunchCallOp);

  /// Creates call to `bindMemRef` for each memref operand.
  void createBindMemRefCalls(LLVM::CallOp vulkanLaunchCallOp,
                             Value vulkanRuntime);

  /// Collects SPIRV attributes from the given `vulkanLaunchCallOp`.
  void collectSPIRVAttributes(LLVM::CallOp vulkanLaunchCallOp);

  /// Deduces a rank from the given 'ptrToMemRefDescriptor`.
  LogicalResult deduceMemRefRank(Value ptrToMemRefDescriptor, uint32_t &rank);

public:
  void runOnModule() override;

private:
  LLVM::LLVMDialect *llvmDialect;
  LLVM::LLVMType llvmFloatType;
  LLVM::LLVMType llvmVoidType;
  LLVM::LLVMType llvmPointerType;
  LLVM::LLVMType llvmInt32Type;
  LLVM::LLVMType llvmInt64Type;
  LLVM::LLVMType llvmMemRef1DFloat;
  LLVM::LLVMType llvmMemRef2DFloat;

  // TODO: Use an associative array to support multiple vulkan launch calls.
  std::pair<StringAttr, StringAttr> spirvAttributes;
};

} // anonymous namespace

void VulkanLaunchFuncToVulkanCallsPass::runOnModule() {
  initializeCachedTypes();

  // Collect SPIR-V attributes such as `spirv_blob` and
  // `spirv_entry_point_name`.
  getModule().walk([this](LLVM::CallOp op) {
    if (isVulkanLaunchCallOp(op))
      collectSPIRVAttributes(op);
  });

  // Convert vulkan launch call op into a sequence of Vulkan runtime calls.
  getModule().walk([this](LLVM::CallOp op) {
    if (isCInterfaceVulkanLaunchCallOp(op))
      translateVulkanLaunchCall(op);
  });
}

void VulkanLaunchFuncToVulkanCallsPass::collectSPIRVAttributes(
    LLVM::CallOp vulkanLaunchCallOp) {
  // Check that `kSPIRVBinary` and `kSPIRVEntryPoint` are present in attributes
  // for the given vulkan launch call.
  auto spirvBlobAttr =
      vulkanLaunchCallOp.getAttrOfType<StringAttr>(kSPIRVBlobAttrName);
  if (!spirvBlobAttr) {
    vulkanLaunchCallOp.emitError()
        << "missing " << kSPIRVBlobAttrName << " attribute";
    return signalPassFailure();
  }

  auto spirvEntryPointNameAttr =
      vulkanLaunchCallOp.getAttrOfType<StringAttr>(kSPIRVEntryPointAttrName);
  if (!spirvEntryPointNameAttr) {
    vulkanLaunchCallOp.emitError()
        << "missing " << kSPIRVEntryPointAttrName << " attribute";
    return signalPassFailure();
  }

  spirvAttributes = std::make_pair(spirvBlobAttr, spirvEntryPointNameAttr);
}

void VulkanLaunchFuncToVulkanCallsPass::createBindMemRefCalls(
    LLVM::CallOp cInterfaceVulkanLaunchCallOp, Value vulkanRuntime) {
  if (cInterfaceVulkanLaunchCallOp.getNumOperands() ==
      gpu::LaunchOp::kNumConfigOperands)
    return;
  OpBuilder builder(cInterfaceVulkanLaunchCallOp);
  Location loc = cInterfaceVulkanLaunchCallOp.getLoc();

  // Create LLVM constant for the descriptor set index.
  // Bind all memrefs to the `0` descriptor set, the same way as `GPUToSPIRV`
  // pass does.
  Value descriptorSet = builder.create<LLVM::ConstantOp>(
      loc, getInt32Type(), builder.getI32IntegerAttr(0));

  for (auto en :
       llvm::enumerate(cInterfaceVulkanLaunchCallOp.getOperands().drop_front(
           gpu::LaunchOp::kNumConfigOperands))) {
    // Create LLVM constant for the descriptor binding index.
    Value descriptorBinding = builder.create<LLVM::ConstantOp>(
        loc, getInt32Type(), builder.getI32IntegerAttr(en.index()));

    auto ptrToMemRefDescriptor = en.value();
    uint32_t rank = 0;
    if (failed(deduceMemRefRank(ptrToMemRefDescriptor, rank))) {
      cInterfaceVulkanLaunchCallOp.emitError()
          << "invalid memref descriptor " << ptrToMemRefDescriptor.getType();
      return signalPassFailure();
    }

    auto symbolName = llvm::formatv("bindMemRef{0}DFloat", rank).str();
    // Create call to `bindMemRef`.
    builder.create<LLVM::CallOp>(
        loc, ArrayRef<Type>{getVoidType()},
        builder.getSymbolRefAttr(
            StringRef(symbolName.data(), symbolName.size())),
        ArrayRef<Value>{vulkanRuntime, descriptorSet, descriptorBinding,
                        ptrToMemRefDescriptor});
  }
}

LogicalResult
VulkanLaunchFuncToVulkanCallsPass::deduceMemRefRank(Value ptrToMemRefDescriptor,
                                                    uint32_t &rank) {
  auto llvmPtrDescriptorTy =
      ptrToMemRefDescriptor.getType().dyn_cast<LLVM::LLVMType>();
  if (!llvmPtrDescriptorTy)
    return failure();

  auto llvmDescriptorTy = llvmPtrDescriptorTy.getPointerElementTy();
  // template <typename Elem, size_t Rank>
  // struct {
  //   Elem *allocated;
  //   Elem *aligned;
  //   int64_t offset;
  //   int64_t sizes[Rank]; // omitted when rank == 0
  //   int64_t strides[Rank]; // omitted when rank == 0
  // };
  if (!llvmDescriptorTy || !llvmDescriptorTy.isStructTy())
    return failure();
  if (llvmDescriptorTy.getStructNumElements() == 3) {
    rank = 0;
    return success();
  }

  rank = llvmDescriptorTy.getStructElementType(3).getArrayNumElements();
  return success();
}

void VulkanLaunchFuncToVulkanCallsPass::declareVulkanFunctions(Location loc) {
  ModuleOp module = getModule();
  OpBuilder builder(module.getBody()->getTerminator());

  if (!module.lookupSymbol(kSetEntryPoint)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, kSetEntryPoint,
        LLVM::LLVMType::getFunctionTy(getVoidType(),
                                      {getPointerType(), getPointerType()},
                                      /*isVarArg=*/false));
  }

  if (!module.lookupSymbol(kSetNumWorkGroups)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, kSetNumWorkGroups,
        LLVM::LLVMType::getFunctionTy(
            getVoidType(),
            {getPointerType(), getInt64Type(), getInt64Type(), getInt64Type()},
            /*isVarArg=*/false));
  }

  if (!module.lookupSymbol(kSetBinaryShader)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, kSetBinaryShader,
        LLVM::LLVMType::getFunctionTy(
            getVoidType(), {getPointerType(), getPointerType(), getInt32Type()},
            /*isVarArg=*/false));
  }

  if (!module.lookupSymbol(kRunOnVulkan)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, kRunOnVulkan,
        LLVM::LLVMType::getFunctionTy(getVoidType(), {getPointerType()},
                                      /*isVarArg=*/false));
  }

  if (!module.lookupSymbol(kBindMemRef1DFloat)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, kBindMemRef1DFloat,
        LLVM::LLVMType::getFunctionTy(getVoidType(),
                                      {getPointerType(), getInt32Type(),
                                       getInt32Type(),
                                       getMemRef1DFloat().getPointerTo()},
                                      /*isVarArg=*/false));
  }

  if (!module.lookupSymbol(kBindMemRef2DFloat)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, kBindMemRef2DFloat,
        LLVM::LLVMType::getFunctionTy(getVoidType(),
                                      {getPointerType(), getInt32Type(),
                                       getInt32Type(),
                                       getMemRef2DFloat().getPointerTo()},
                                      /*isVarArg=*/false));
  }

  if (!module.lookupSymbol(kInitVulkan)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, kInitVulkan,
        LLVM::LLVMType::getFunctionTy(getPointerType(), {},
                                      /*isVarArg=*/false));
  }

  if (!module.lookupSymbol(kDeinitVulkan)) {
    builder.create<LLVM::LLVMFuncOp>(
        loc, kDeinitVulkan,
        LLVM::LLVMType::getFunctionTy(getVoidType(), {getPointerType()},
                                      /*isVarArg=*/false));
  }
}

Value VulkanLaunchFuncToVulkanCallsPass::createEntryPointNameConstant(
    StringRef name, Location loc, OpBuilder &builder) {
  SmallString<16> shaderName(name.begin(), name.end());
  // Append `\0` to follow C style string given that LLVM::createGlobalString()
  // won't handle this directly for us.
  shaderName.push_back('\0');

  std::string entryPointGlobalName = (name + "_spv_entry_point_name").str();
  return LLVM::createGlobalString(loc, builder, entryPointGlobalName,
                                  shaderName, LLVM::Linkage::Internal,
                                  getLLVMDialect());
}

void VulkanLaunchFuncToVulkanCallsPass::translateVulkanLaunchCall(
    LLVM::CallOp cInterfaceVulkanLaunchCallOp) {
  OpBuilder builder(cInterfaceVulkanLaunchCallOp);
  Location loc = cInterfaceVulkanLaunchCallOp.getLoc();
  // Create call to `initVulkan`.
  auto initVulkanCall = builder.create<LLVM::CallOp>(
      loc, ArrayRef<Type>{getPointerType()},
      builder.getSymbolRefAttr(kInitVulkan), ArrayRef<Value>{});
  // The result of `initVulkan` function is a pointer to Vulkan runtime, we
  // need to pass that pointer to each Vulkan runtime call.
  auto vulkanRuntime = initVulkanCall.getResult(0);

  // Create LLVM global with SPIR-V binary data, so we can pass a pointer with
  // that data to runtime call.
  Value ptrToSPIRVBinary = LLVM::createGlobalString(
      loc, builder, kSPIRVBinary, spirvAttributes.first.getValue(),
      LLVM::Linkage::Internal, getLLVMDialect());

  // Create LLVM constant for the size of SPIR-V binary shader.
  Value binarySize = builder.create<LLVM::ConstantOp>(
      loc, getInt32Type(),
      builder.getI32IntegerAttr(spirvAttributes.first.getValue().size()));

  // Create call to `bindMemRef` for each memref operand.
  createBindMemRefCalls(cInterfaceVulkanLaunchCallOp, vulkanRuntime);

  // Create call to `setBinaryShader` runtime function with the given pointer to
  // SPIR-V binary and binary size.
  builder.create<LLVM::CallOp>(
      loc, ArrayRef<Type>{getVoidType()},
      builder.getSymbolRefAttr(kSetBinaryShader),
      ArrayRef<Value>{vulkanRuntime, ptrToSPIRVBinary, binarySize});
  // Create LLVM global with entry point name.
  Value entryPointName = createEntryPointNameConstant(
      spirvAttributes.second.getValue(), loc, builder);
  // Create call to `setEntryPoint` runtime function with the given pointer to
  // entry point name.
  builder.create<LLVM::CallOp>(loc, ArrayRef<Type>{getVoidType()},
                               builder.getSymbolRefAttr(kSetEntryPoint),
                               ArrayRef<Value>{vulkanRuntime, entryPointName});

  // Create number of local workgroup for each dimension.
  builder.create<LLVM::CallOp>(
      loc, ArrayRef<Type>{getVoidType()},
      builder.getSymbolRefAttr(kSetNumWorkGroups),
      ArrayRef<Value>{vulkanRuntime, cInterfaceVulkanLaunchCallOp.getOperand(0),
                      cInterfaceVulkanLaunchCallOp.getOperand(1),
                      cInterfaceVulkanLaunchCallOp.getOperand(2)});

  // Create call to `runOnVulkan` runtime function.
  builder.create<LLVM::CallOp>(loc, ArrayRef<Type>{getVoidType()},
                               builder.getSymbolRefAttr(kRunOnVulkan),
                               ArrayRef<Value>{vulkanRuntime});

  // Create call to 'deinitVulkan' runtime function.
  builder.create<LLVM::CallOp>(loc, ArrayRef<Type>{getVoidType()},
                               builder.getSymbolRefAttr(kDeinitVulkan),
                               ArrayRef<Value>{vulkanRuntime});

  // Declare runtime functions.
  declareVulkanFunctions(loc);

  cInterfaceVulkanLaunchCallOp.erase();
}

std::unique_ptr<mlir::OpPassBase<mlir::ModuleOp>>
mlir::createConvertVulkanLaunchFuncToVulkanCallsPass() {
  return std::make_unique<VulkanLaunchFuncToVulkanCallsPass>();
}

static PassRegistration<VulkanLaunchFuncToVulkanCallsPass>
    pass("launch-func-to-vulkan",
         "Convert vulkanLaunch external call to Vulkan runtime external calls");
