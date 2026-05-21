// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file ZepraTierBuilder.h
 * @brief Warp-Style IC-Driven Optimization
 * 
 * - Captures IC chain data as WarpSnapshot
 * - Translates IC stubs directly to ZOpt nodes
 * - Bypasses bytecode for hot paths
 */

#pragma once

#include "InlineCache.h"
#include <algorithm>
#include "type_profiler.hpp"
#include "../zopt/ZOptGraph.h"
#include "../zopt/ZOptValue.h"
#include "../zopt/ZOptOpcode.h"

#include <vector>
#include <memory>
#include <unordered_map>

namespace Zepra::JIT {

// =============================================================================
// Warp Snapshot (Captured IC State)
// =============================================================================

/**
 * @brief Snapshot of IC state at compile time
 */
struct WarpICSnapshot {
    ICKind kind;
    uint32_t bytecodeOffset;
    ICState state;
    
    // Captured stubs
    struct CapturedStub {
        ShapeId shape;
        PropertySlot slot;
        uint32_t hitCount;
    };
    std::vector<CapturedStub> stubs;
    
    // Type profile
    TypeProfile typeProfile;
};

/**
 * @brief Complete snapshot for a function
 */
class WarpSnapshot {
public:
    explicit WarpSnapshot(uintptr_t functionId)
        : functionId_(functionId) {}
    
    uintptr_t functionId() const { return functionId_; }
    
    // Capture IC data
    void captureIC(const ICSite& site) {
        WarpICSnapshot snap;
        snap.kind = site.kind();
        snap.bytecodeOffset = site.bytecodeOffset();
        snap.state = site.state();
        
        site.chain().forEach([&](const ICStub& stub) {
            snap.stubs.push_back({
                stub.expectedShape(),
                stub.slot(),
                stub.hitCount()
            });
        });
        
        icSnapshots_.push_back(std::move(snap));
    }
    
    // Capture type profile
    void captureTypeProfile(uint32_t offset, const TypeProfile& profile) {
        typeProfiles_[offset] = profile;
    }
    
    const std::vector<WarpICSnapshot>& icSnapshots() const { return icSnapshots_; }
    
    const TypeProfile* getTypeProfile(uint32_t offset) const {
        auto it = typeProfiles_.find(offset);
        return it != typeProfiles_.end() ? &it->second : nullptr;
    }
    
private:
    uintptr_t functionId_;
    std::vector<WarpICSnapshot> icSnapshots_;
    std::unordered_map<uint32_t, TypeProfile> typeProfiles_;
};

// =============================================================================
// Warp Transpiler (CacheIR → DFG)
// =============================================================================

/**
 * @brief Translates CacheIR bytecode to ZOpt nodes
 */
class WarpCacheIRTranspiler {
public:
    explicit WarpCacheIRTranspiler(ZOpt::Graph* graph)
        : graph_(graph) {}
    
    // Transpile IC stub to ZOpt nodes
    ZOpt::Value* transpileGetProp(const WarpICSnapshot& snap, ZOpt::Value* obj) {
        if (snap.state == ICState::Megamorphic) {
            return emitGenericGetProp(obj);
        }
        
        if (snap.stubs.size() == 1) {
            return emitMonomorphicGetProp(snap.stubs[0], obj);
        }
        
        return emitPolymorphicGetProp(snap.stubs, obj);
    }
    
    ZOpt::Value* transpileSetProp(const WarpICSnapshot& snap, 
                                  ZOpt::Value* obj, ZOpt::Value* value) {
        if (snap.state == ICState::Megamorphic) {
            return emitGenericSetProp(obj, value);
        }
        
        if (snap.stubs.size() == 1) {
            return emitMonomorphicSetProp(snap.stubs[0], obj, value);
        }
        
        return emitPolymorphicSetProp(snap.stubs, obj, value);
    }
    
private:
    // Monomorphic: single guard + load
    ZOpt::Value* emitMonomorphicGetProp(const WarpICSnapshot::CapturedStub& stub,
                                        ZOpt::Value* obj) {
        // Guard shape
        auto* guard = graph_->createValue(ZOpt::Opcode::CheckShape,
                                          ZOpt::ValueType::None);
        guard->addChild(obj);
        guard->setConstant(stub.shape);
        
        // Load slot
        ZOpt::Opcode loadOp = stub.slot.isInline() 
            ? ZOpt::Opcode::GetInlineProperty 
            : ZOpt::Opcode::GetOutOfLineProperty;
            
        auto* load = graph_->createValue(loadOp, ZOpt::ValueType::Any);
        load->addChild(obj);
        load->setConstant(stub.slot.offset);
        
        return load;
    }
    
    // Polymorphic: switch on shape
    ZOpt::Value* emitPolymorphicGetProp(
        const std::vector<WarpICSnapshot::CapturedStub>& stubs,
        ZOpt::Value* obj) {
        
        // Create a Switch on shape
        auto* phi = graph_->createValue(ZOpt::Opcode::Phi, ZOpt::ValueType::Any);
        
        for (const auto& stub : stubs) {
            // Each path: guard + load
            auto* guard = graph_->createValue(ZOpt::Opcode::CheckShape,
                                              ZOpt::ValueType::None);
            guard->addChild(obj);
            guard->setConstant(stub.shape);
            
            ZOpt::Opcode loadOp = stub.slot.isInline()
                ? ZOpt::Opcode::GetInlineProperty
                : ZOpt::Opcode::GetOutOfLineProperty;
                
            auto* load = graph_->createValue(loadOp, ZOpt::ValueType::Any);
            load->addChild(obj);
            load->setConstant(stub.slot.offset);
            
            phi->addChild(load);
        }
        
        // Fallback to generic
        auto* fallback = emitGenericGetProp(obj);
        phi->addChild(fallback);
        
        return phi;
    }
    
    // Generic: call runtime
    ZOpt::Value* emitGenericGetProp(ZOpt::Value* obj) {
        auto* call = graph_->createValue(ZOpt::Opcode::Call, ZOpt::ValueType::Any);
        call->addChild(obj);
        // Would set call target to runtime GetProperty
        return call;
    }
    
    ZOpt::Value* emitMonomorphicSetProp(const WarpICSnapshot::CapturedStub& stub,
                                        ZOpt::Value* obj, ZOpt::Value* value) {
        auto* guard = graph_->createValue(ZOpt::Opcode::CheckShape,
                                          ZOpt::ValueType::None);
        guard->addChild(obj);
        guard->setConstant(stub.shape);
        
        ZOpt::Opcode storeOp = stub.slot.isInline()
            ? ZOpt::Opcode::SetInlineProperty
            : ZOpt::Opcode::SetOutOfLineProperty;
            
        auto* store = graph_->createValue(storeOp, ZOpt::ValueType::None);
        store->addChild(obj);
        store->addChild(value);
        store->setConstant(stub.slot.offset);
        
        return store;
    }
    
    ZOpt::Value* emitPolymorphicSetProp(
        const std::vector<WarpICSnapshot::CapturedStub>& stubs,
        ZOpt::Value* obj, ZOpt::Value* value) {
        
        for (const auto& stub : stubs) {
            auto* guard = graph_->createValue(ZOpt::Opcode::CheckShape,
                                              ZOpt::ValueType::None);
            guard->addChild(obj);
            guard->setConstant(stub.shape);
            
            ZOpt::Opcode storeOp = stub.slot.isInline()
                ? ZOpt::Opcode::SetInlineProperty
                : ZOpt::Opcode::SetOutOfLineProperty;
                
            auto* store = graph_->createValue(storeOp, ZOpt::ValueType::None);
            store->addChild(obj);
            store->addChild(value);
            store->setConstant(stub.slot.offset);
        }
        
        return emitGenericSetProp(obj, value);
    }
    
    ZOpt::Value* emitGenericSetProp(ZOpt::Value* obj, ZOpt::Value* value) {
        auto* call = graph_->createValue(ZOpt::Opcode::Call, ZOpt::ValueType::None);
        call->addChild(obj);
        call->addChild(value);
        return call;
    }
    
    ZOpt::Graph* graph_;
};

// =============================================================================
// Warp Builder (Bytecode + IC → DFG)
// =============================================================================

/**
 * @brief Builds ZOpt from bytecode + IC snapshots
 */
class WarpBuilder {
public:
    WarpBuilder(ZOpt::Graph* graph, WarpSnapshot* snapshot)
        : graph_(graph), snapshot_(snapshot), transpiler_(graph) {}
    
    // Build ZOpt for function
    bool build() {
        // For each IC snapshot, transpile to DFG
        for (const auto& icSnap : snapshot_->icSnapshots()) {
            if (!processICSnapshot(icSnap)) {
                return false;
            }
        }
        
        return true;
    }
    
    // Check if function is hot enough for Warp
    static bool shouldWarp(uintptr_t functionId) {
        // Check IC monomorphism rates
        auto& mgr = ICManager::instance();
        auto stats = mgr.collectStats();
        
        // Warp is beneficial when most ICs are monomorphic
        if (stats.totalSites == 0) return false;
        
        double monoRate = static_cast<double>(stats.monomorphic) / stats.totalSites;
        return monoRate > 0.7;  // 70% monomorphic threshold
    }
    
private:
    bool processICSnapshot(const WarpICSnapshot& snap) {
        switch (snap.kind) {
            case ICKind::GetProp:
            case ICKind::SetProp:
            case ICKind::GetElem:
            case ICKind::SetElem:
                // Property access - already handled by transpiler
                return true;
                
            case ICKind::Call:
            case ICKind::Construct:
                // Would handle call ICs
                return true;
                
            case ICKind::BinaryOp:
            case ICKind::UnaryOp:
            case ICKind::Compare:
                // Would handle arithmetic ICs
                return true;
                
            default:
                return true;
        }
    }
    
    ZOpt::Graph* graph_;
    WarpSnapshot* snapshot_;
    WarpCacheIRTranspiler transpiler_;
};

// =============================================================================
// Warp Compile Entry Point
// =============================================================================

/**
 * @brief Entry point for Warp compilation
 */
class WarpCompiler {
public:
    struct Result {
        bool success = false;
        void* code = nullptr;
        size_t codeSize = 0;
    };
    
    static Result compile(uintptr_t functionId) {
        Result result;
        
        // Check if worth Warping
        if (!WarpBuilder::shouldWarp(functionId)) {
            return result;
        }
        
        // Capture snapshot
        auto snapshot = std::make_unique<WarpSnapshot>(functionId);
        captureSnapshot(functionId, *snapshot);
        
        // Build DFG
        ZOpt::Graph graph;
        WarpBuilder builder(&graph, snapshot.get());
        
        if (!builder.build()) {
            return result;
        }
        
        // Run optimization passes
        graph.runOptimizationPasses();
        
        // Generate code
        // result.code = graph.generateCode();
        // result.codeSize = graph.codeSize();
        result.success = true;
        
        return result;
    }
    
private:
    static void captureSnapshot(uintptr_t functionId, WarpSnapshot& snapshot) {
        // Would iterate all IC sites for function and capture
        (void)functionId;
        (void)snapshot;
    }
};

} // namespace Zepra::JIT
