#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

namespace clickhle::vc2 {

// Emit a contiguous, horizontal lane path before the general kernel. Each
// halfword uses two byte planes; crossing either plane's row boundary must
// retain the general wrapping path. LLVM legalizes these vectors for the host.
inline void emit_horizontal_simd(llvm::IRBuilder<> &build, llvm::Function &function,
                                 unsigned opcode, unsigned widths, bool broadcast = false) {
    switch (opcode) {
    case 4: case 8: case 9: case 10: case 11: case 12: case 13: case 14:
    case 16: case 17: case 18: case 26: case 27: case 28: case 29: case 30: case 37:
        break;
    default:
        return;
    }
    auto &context = function.getContext();
    auto *i8 = build.getInt8Ty();
    auto *i32 = build.getInt32Ty();
    auto *bytes = llvm::FixedVectorType::get(i8, 16);
    auto *words = llvm::FixedVectorType::get(build.getInt16Ty(), 16);
    auto *lanes = llvm::FixedVectorType::get(i32, 16);
    auto constant = [&](int value) {
        return llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(16),
                                             llvm::ConstantInt::getSigned(i32, value));
    };
    auto *vrf = function.getArg(0);
    auto *destination = function.getArg(1);
    auto *left = function.getArg(2);
    auto *right = function.getArg(3);
    auto contiguous = [&](llvm::Value *base, bool halfword) -> llvm::Value * {
        auto *column = build.CreateAnd(base, 63);
        auto *valid = build.CreateICmpULE(column, build.getInt32(48));
        if (halfword) {
            auto *high = build.CreateAnd(build.CreateAdd(column, build.getInt32(16)), 63);
            valid = build.CreateAnd(valid, build.CreateICmpULE(high, build.getInt32(48)));
        }
        return valid;
    };
    auto *valid = contiguous(destination, widths & 1);
    if (!broadcast) valid = build.CreateAnd(valid, contiguous(right, widths & 4));
    if (opcode != 4) valid = build.CreateAnd(valid, contiguous(left, widths & 2));
    auto *simd = llvm::BasicBlock::Create(context, "horizontal-simd", &function);
    auto *general = llvm::BasicBlock::Create(context, "wrapped-lanes", &function);
    build.CreateCondBr(valid, simd, general);
    build.SetInsertPoint(simd);
    auto high_base = [&](llvm::Value *base) {
        return build.CreateOr(build.CreateAnd(base, ~63u),
            build.CreateAnd(build.CreateAdd(base, build.getInt32(16)), 63));
    };
    auto pointer = [&](llvm::Value *base) {
        return build.CreateInBoundsGEP(i8, vrf, base);
    };
    auto load = [&](llvm::Value *base, bool halfword) -> llvm::Value * {
        auto *low = build.CreateAlignedLoad(bytes, pointer(base), llvm::Align(1));
        if (!halfword) return build.CreateZExt(low, lanes);
        auto *high = build.CreateAlignedLoad(bytes, pointer(high_base(base)), llvm::Align(1));
        auto *word = build.CreateOr(build.CreateZExt(low, words),
                                   build.CreateShl(build.CreateZExt(high, words), 8));
        return build.CreateSExt(word, lanes);
    };
    llvm::Value *a = opcode == 4 ? constant(0) : load(left, widths & 2);
    auto *b = broadcast ? build.CreateVectorSplat(16, right) : load(right, widths & 4);
    llvm::Value *value = nullptr;
    switch (opcode) {
    case 4: value = b; break;
    case 8: case 9: value = build.CreateAdd(a, b); break;
    case 10: case 11: value = build.CreateSub(a, b); break;
    case 12: case 13: value = build.CreateSub(b, a); break;
    case 14: value = build.CreateMul(a, b); break;
    case 16: case 17: value = build.CreateAShr(build.CreateMul(a, b), constant(8)); break;
    case 18: value = build.CreateAShr(build.CreateMul(a, b), constant(16)); break;
    case 26: value = build.CreateAShr(a, build.CreateAnd(b, constant(31))); break;
    case 27:
        value = build.CreateLShr(build.CreateAnd(a, constant((widths & 2) ? 65535 : 255)),
                                 build.CreateAnd(b, constant(31)));
        break;
    case 28: case 37:
        value = build.CreateSelect(build.CreateICmpSLT(a, b), a, b);
        if (opcode == 37)
            value = build.CreateSelect(build.CreateICmpSLT(value, constant(0)), constant(0), value);
        break;
    case 29: value = build.CreateSelect(build.CreateICmpSGT(a, b), a, b); break;
    case 30:
        value = build.CreateSelect(build.CreateICmpSGE(a, b),
                                    build.CreateSub(a, b), build.CreateSub(b, a));
        break;
    }
    if (opcode == 9 || opcode == 11 || opcode == 13 || opcode == 17) {
        auto *minimum = constant((widths & 1) ? -32768 : 0);
        auto *maximum = constant((widths & 1) ? 32767 : 255);
        value = build.CreateSelect(build.CreateICmpSLT(value, minimum), minimum, value);
        value = build.CreateSelect(build.CreateICmpSGT(value, maximum), maximum, value);
    }
    // Finish all source reads before either store: planes and operands may alias.
    build.CreateAlignedStore(build.CreateTrunc(value, bytes), pointer(destination), llvm::Align(1));
    if (widths & 1)
        build.CreateAlignedStore(build.CreateTrunc(build.CreateLShr(value, constant(8)), bytes),
                                  pointer(high_base(destination)), llvm::Align(1));
    build.CreateRetVoid();
    build.SetInsertPoint(general);
}

// Horizontal 16-bit ADD/SETF keeps signed sums for N and unsigned low-half
// sums for the verified low-carry form. Wrapped planes use the scalar emitter.
inline void emit_add_flags_simd(llvm::IRBuilder<> &build, llvm::Function &function,
                                bool lowcarry) {
    auto &context = function.getContext();
    auto *bytes = llvm::FixedVectorType::get(build.getInt8Ty(), 16);
    auto *halves = llvm::FixedVectorType::get(build.getInt16Ty(), 16);
    auto *lanes = llvm::FixedVectorType::get(build.getInt32Ty(), 16);
    auto constant = [&](int value) {
        return llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(16),
                                             build.getInt32(static_cast<uint32_t>(value)));
    };
    auto high = [&](llvm::Value *base) {
        return build.CreateOr(build.CreateAnd(base, ~63u),
            build.CreateAnd(build.CreateAdd(base, build.getInt32(16)), 63));
    };
    auto pointer = [&](llvm::Value *base) {
        return build.CreateInBoundsGEP(build.getInt8Ty(), function.getArg(0), base);
    };
    llvm::Value *valid = build.getTrue();
    for (unsigned arg = 1; arg <= 3; ++arg) {
        auto *base = function.getArg(arg);
        valid = build.CreateAnd(valid, build.CreateICmpULE(build.CreateAnd(base, 63), build.getInt32(48)));
        valid = build.CreateAnd(valid, build.CreateICmpULE(build.CreateAnd(high(base), 63), build.getInt32(48)));
    }
    auto *simd = llvm::BasicBlock::Create(context, "add-flags-simd", &function);
    auto *wrapped = llvm::BasicBlock::Create(context, "wrapped-add-flags", &function);
    build.CreateCondBr(valid, simd, wrapped);
    build.SetInsertPoint(simd);
    auto load = [&](llvm::Value *base) {
        auto *lo = build.CreateZExt(build.CreateAlignedLoad(bytes, pointer(base), llvm::Align(1)), halves);
        auto *hi = build.CreateZExt(build.CreateAlignedLoad(bytes, pointer(high(base)), llvm::Align(1)), halves);
        return build.CreateSExt(build.CreateOr(lo, build.CreateShl(hi, 8)), lanes);
    };
    auto *a = load(function.getArg(2));
    auto *b = load(function.getArg(3));
    auto *sum = build.CreateAdd(a, b);
    auto *negative = build.CreateICmpSLT(sum, constant(0));
    auto *zero = build.CreateICmpEQ(build.CreateAnd(sum, constant(65535)), constant(0));
    auto *carry = lowcarry ? build.CreateICmpUGT(build.CreateAdd(
        build.CreateAnd(a, constant(65535)), build.CreateAnd(b, constant(65535))), constant(65535))
        : build.CreateOr(negative, build.CreateICmpSGT(sum, constant(65535)));
    build.CreateAlignedStore(build.CreateTrunc(sum, bytes), pointer(function.getArg(1)), llvm::Align(1));
    build.CreateAlignedStore(build.CreateTrunc(build.CreateLShr(sum, constant(8)), bytes),
                              pointer(high(function.getArg(1))), llvm::Align(1));
    build.CreateAlignedStore(build.CreateZExt(zero, bytes), function.getArg(4), llvm::Align(1));
    build.CreateAlignedStore(build.CreateZExt(negative, bytes), function.getArg(5), llvm::Align(1));
    build.CreateAlignedStore(build.CreateZExt(carry, bytes), function.getArg(6), llvm::Align(1));
    build.CreateRetVoid();
    build.SetInsertPoint(wrapped);
}

} // namespace clickhle::vc2
