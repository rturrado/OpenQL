/**
 * @file    arch/cc/pass/gen/vq1asm/detail/codegen.cc
 * @date    201810xx
 * @author  Wouter Vlothuizen (wouter.vlothuizen@tno.nl)
 * @brief   code generator backend for the Central Controller
 * @note    here we don't check whether the sequence of calling code generator
 *          functions is correct
 */

#include "codegen.h"

#include "ql/version.h"
#include "ql/com/options.h"
#include "ql/ir/describe.h"
#include "ql/ir/ops.h"

#include <iosfwd>

namespace ql {
namespace arch {
namespace cc {
namespace pass {
namespace gen {
namespace vq1asm {
namespace detail {

using namespace utils;

// helpers for label generation.
static Str to_start(const Str &base) { return base + "_start"; };
static Str to_end(const Str &base) { return base + "_end"; };
static Str to_ifbranch(const Str &base, Int branch) { return QL_SS2S(base << "_" << branch); }
static Str as_label(const Str &label) { return label + ":"; }
static Str as_target(const Str &label) { return "@" + label; }

// helpers
static void check_int_literal(const ir::IntLiteral &ilit, Int bottomRoom=0, Int headRoom=0) {
    if(ilit.value-bottomRoom < 0) {
        QL_INPUT_ERROR("CC backend cannot handle negative integer literals: value=" << ilit.value << ", bottomRoom=" << bottomRoom);
    }
    if(ilit.value >= (1L<<32) - 1 - headRoom) {
        QL_INPUT_ERROR("CC backend requires integer literals limited to 32 bits: value=" << ilit.value << ", headRoom=" << headRoom);
    }
}


typedef struct {
    ConditionType cond_type;
    utils::Vec<utils::UInt> cond_operands;
} tInstructionCondition;

/**
 * Decode the expression for a conditional instruction into the old format as used for the API. Eventually this will have
 * to be changed, but as long as the CC can handle expressions with 2 variables only this covers all we need.
 */
static tInstructionCondition decode_condition(const OperandContext &operandContext, const ir::ExpressionRef &condition) {
    ConditionType cond_type;
    utils::Vec<utils::UInt> cond_operands;

    try {
        if (auto blit = condition->as_bit_literal()) {
            if (blit->value) {
                cond_type = ConditionType::ALWAYS;
            } else {
                cond_type = ConditionType::NEVER;
            }
        } else if (condition->as_reference()) {
            cond_operands.push_back(operandContext.convert_breg_reference(condition));
            cond_type = ConditionType::UNARY;
        } else if (auto fn = condition->as_function_call()) {
            if (
                fn->function_type->name == "operator!" ||
                fn->function_type->name == "operator~"
            ) {
                CHECK_COMPAT(fn->operands.size() == 1, "unsupported condition function");
                if (fn->operands[0]->as_reference()) {
                    cond_operands.push_back(operandContext.convert_breg_reference(fn->operands[0]));
                    cond_type = ConditionType::NOT;
                } else if (auto fn2 = fn->operands[0]->as_function_call()) {
                    CHECK_COMPAT(fn2->operands.size() == 2, "unsupported condition function");
                    cond_operands.push_back(operandContext.convert_breg_reference(fn2->operands[0]));
                    cond_operands.push_back(operandContext.convert_breg_reference(fn2->operands[1]));
                    if (
                        fn2->function_type->name == "operator&" ||
                        fn2->function_type->name == "operator&&"
                    ) {
                        cond_type = ConditionType::NAND;
                    } else if (
                        fn2->function_type->name == "operator|" ||
                        fn2->function_type->name == "operator||"
                    ) {
                        cond_type = ConditionType::NOR;
                    } else if (
                        fn2->function_type->name == "operator^" ||
                        fn2->function_type->name == "operator^^" ||
                        fn2->function_type->name == "operator!="
                    ) {
                        cond_type = ConditionType::NXOR;
                    } else if (
                        fn2->function_type->name == "operator=="
                    ) {
                        cond_type = ConditionType::XOR;
                    } else {
                        QL_ICE("unsupported gate condition");
                    }
                } else {
                    QL_ICE("unsupported gate condition");
                }
            } else {
                CHECK_COMPAT(fn->operands.size() == 2, "unsupported condition function");
                cond_operands.push_back(operandContext.convert_breg_reference(fn->operands[0]));
                cond_operands.push_back(operandContext.convert_breg_reference(fn->operands[1]));
                if (
                    fn->function_type->name == "operator&" ||
                    fn->function_type->name == "operator&&"
                ) {
                    cond_type = ConditionType::AND;
                } else if (
                    fn->function_type->name == "operator|" ||
                    fn->function_type->name == "operator||"
                ) {
                    cond_type = ConditionType::OR;
                } else if (
                    fn->function_type->name == "operator^" ||
                    fn->function_type->name == "operator^^" ||
                    fn->function_type->name == "operator!="
                ) {
                    cond_type = ConditionType::XOR;
                } else if (
                    fn->function_type->name == "operator=="
                ) {
                    cond_type = ConditionType::NXOR;
                } else {
                    QL_ICE("unsupported condition function");
                }
            }
        } else {
            QL_ICE("unsupported condition expression");
        }
    } catch (utils::Exception &e) {
        e.add_context("in gate condition", true);
        throw;
    }
    return {cond_type, cond_operands};
}


// Static helper function for bundleFinish()
typedef struct {
    tDigital groupDigOut;    // codeword/mask fragment for this group
    Str comment;            // comment for instruction stream
} CalcGroupDigOut;

static CalcGroupDigOut calcGroupDigOut(
        UInt instrIdx,
        UInt group,
        UInt nrGroups,
        const Settings::InstrumentControl &ic,
        tCodeword staticCodewordOverride
) {
    CalcGroupDigOut ret{0, ""};

    // determine control mode group FIXME: more explanation
    Int controlModeGroup = -1;
    if (ic.controlModeGroupCnt == 0) {
        QL_JSON_ERROR("'control_bits' not defined or empty in 'control_modes/" << ic.refControlMode <<"'");
#if OPT_VECTOR_MODE
    } else if (ic.controlModeGroupCnt == 1) {                   // vector mode: group addresses channel within vector
        controlModeGroup = 0;
#endif
    } else if (group < ic.controlModeGroupCnt) {                // normal mode: group selects control group
        controlModeGroup = group;
    } else {
        // NB: this actually an error in program logic
        QL_JSON_ERROR(
            "instrument '" << ic.ii.instrumentName
            << "' uses " << nrGroups
            << " groups, but control mode '" << ic.refControlMode
            << "' only defines " << ic.controlModeGroupCnt
            << " groups in 'control_bits'"
        );
    }

    // get number of control bits for group
    const Json &groupControlBits = ic.controlMode["control_bits"][controlModeGroup];    // NB: tests above guarantee existence
    QL_DOUT(
        "instrumentName=" << ic.ii.instrumentName
        << ", slot=" << ic.ii.slot
        << ", control mode group=" << controlModeGroup
        << ", group control bits: " << groupControlBits
    );
    UInt nrGroupControlBits = groupControlBits.size();


    // calculate digital output for group
    if (nrGroupControlBits == 1) {       // single bit, implying this is a mask (not code word)
        ret.groupDigOut |= 1ul << groupControlBits[0].get<Int>();     // NB: we assume the mask is active high, which is correct for VSM and UHF-QC
        // FIXME: check controlModeGroup vs group
    } else if (nrGroupControlBits > 1) {                 // > 1 bit, implying code word
#if OPT_VECTOR_MODE
        //  allow single code word for vector of groups. FIXME: requires looking at all sd.signal before assigning code word
        if (group != controlModeGroup) {
            // FIXME: unfinished work on vector mode
        }
#endif

        // find or assign code word
        tCodeword codeword = 0;
        Bool codewordOverriden = false;
#if OPT_SUPPORT_STATIC_CODEWORDS
        codeword = staticCodewordOverride;
        codewordOverriden = true;
#else
        codeword = assignCodeword(ic.ii.instrumentName, instrIdx, group);
#endif

        // convert codeword to digOut
        for (size_t idx=0; idx<nrGroupControlBits; idx++) {
            Int codeWordBit = nrGroupControlBits - 1 - idx;    // NB: groupControlBits defines MSB..LSB
            if (codeword & (1ul << codeWordBit)) {
                ret.groupDigOut |= 1ul << groupControlBits[idx].get<Int>();
            }
        }

        ret.comment = QL_SS2S(
            "  # slot=" << ic.ii.slot
            << ", instrument='" << ic.ii.instrumentName << "'"
            << ", group=" << group
            << ": codeword=" << codeword
            << std::string(codewordOverriden ? " (static override)" : "")
            << ": groupDigOut=0x" << std::hex << std::setfill('0') << std::setw(8) << ret.groupDigOut
        );
    } else {    // nrGroupControlBits < 1
        QL_JSON_ERROR(
            "key 'control_bits' empty for group " << controlModeGroup
            << " on instrument '" << ic.ii.instrumentName << "'"
        );
    }

    // add trigger to digOut
    UInt nrTriggerBits = ic.controlMode["trigger_bits"].size();
    if (nrTriggerBits == 0) {                                   // no trigger
        // do nothing
    } else if (nrTriggerBits == 1) {                            // single trigger for all groups (NB: will possibly assigned multiple times)
        ret.groupDigOut |= 1ul << ic.controlMode["trigger_bits"][0].get<Int>();
#if 1    // FIXME: hotfix for QWG, implement properly
    } else if(nrTriggerBits == 2) {
        ret.groupDigOut |= 1ul << ic.controlMode["trigger_bits"][0].get<Int>();
        ret.groupDigOut |= 1ul << ic.controlMode["trigger_bits"][1].get<Int>();
#endif
#if 1   // FIXME: trigger per group
    } else if(nrTriggerBits == nrGroups) {                      // trigger per group
        ret.groupDigOut |= 1ul << ic.controlMode["trigger_bits"][group].get<Int>();
#endif
    } else {
        QL_JSON_ERROR(
            "instrument '" << ic.ii.instrumentName
            << "' uses " << nrGroups
            << " groups, but control mode '" << ic.refControlMode
            << "' defines " << nrTriggerBits
            << " trigger bits in 'trigger_bits' (must be 1 or #groups)"
        );
    }

    return ret;
}






Codegen::Codegen(const ir::Ref &ir, const OptionsRef &options)
    : ir(ir)
    , options(options)
    , operandContext(ir)
{
    // NB: a new Backend is instantiated per call to compile, and
    // as a result also a Codegen, so we don't need to cleanup

    settings.loadBackendSettings(ir->platform);

    // optionally preload codewordTable
    Str map_input_file = options->map_input_file;
    if (!map_input_file.empty()) {
        QL_DOUT("loading map_input_file='" << map_input_file << "'");
        Json map = load_json(map_input_file);
        codewordTable = map["codeword_table"];      // FIXME: use json_get
        mapPreloaded = true;
    }

    // show instruments that can produce feedback results
    for (UInt instrIdx = 0; instrIdx < settings.getInstrumentsSize(); instrIdx++) {
        const Settings::InstrumentControl ic = settings.getInstrumentControl(instrIdx);
        if (QL_JSON_EXISTS(ic.controlMode, "result_bits")) {  // this instrument mode produces results (i.e. it is a measurement device)
            QL_IOUT("instrument '" << ic.ii.instrumentName << "' (index " << instrIdx << ") is used for feedback");
        }
    }
}

/************************************************************************\
| Generic
\************************************************************************/

Str Codegen::getProgram() {
    return codeSection.str() + dp.getDatapathSection();
}

Str Codegen::getMap() {
    Json map;

    map["note"] = "generated by OpenQL CC backend version " CC_BACKEND_VERSION_STRING;
    map["codeword_table"] = codewordTable;
    return QL_SS2S(std::setw(4) << map << std::endl);
}

/************************************************************************\
| 'Program' level functions
\************************************************************************/

void Codegen::programStart(const Str &progName) {
    emitProgramStart(progName);

    dp.programStart();

    // Determine number of qubits.
    utils::UInt num_qubits;
    if (ir->platform->qubits->shape.size() == 1) {
        num_qubits = ir->platform->qubits->shape[0];
    } else {
        QL_USER_ERROR("main qubit register has wrong dimensionality");
    };

    // Get cycle time from old Platform (NB: in new Platform, all durations are in quantum cycles, not ns).
    auto &json = ir->platform->data.data;
    QL_JSON_ASSERT(json, "hardware_settings", "hardware_settings");
    auto hardware_settings = json["hardware_settings"];
    QL_JSON_ASSERT(hardware_settings, "cycle_time", "hardware_settings/cycle_time");
    UInt cycle_time = hardware_settings["cycle_time"];

    vcd.programStart(num_qubits, cycle_time, MAX_GROUPS, settings);
}


void Codegen::programFinish(const Str &progName) {
    emitProgramFinish();

    dp.programFinish();

    vcd.programFinish(options->output_prefix + ".vcd");
}

/************************************************************************\
| 'Block' (fka 'Kernel', this name stays relevant as it is used by the
| API) level functions
\************************************************************************/

void Codegen::block_start(const Str &block_name, Int depth) {
    this->depth = depth;
    if(depth == 0) {
        comment("");    // white space before top level block
    }
    comment(QL_SS2S("### Block: '" << block_name << "'"));
    zero(lastEndCycle); // NB; new IR starts counting at zero
}

void Codegen::block_finish(const Str &block_name, UInt durationInCycles, Int depth) {
    comment(QL_SS2S("### Block end: '" << block_name << "'"));
    vcd.kernelFinish(block_name, durationInCycles);

    // unindent, unless at top (in which case nothing follows)
    this->depth = depth>0 ? depth-1 : 0;
}

/************************************************************************\
| 'Bundle' level functions. Although the new IR no longer organizes
| instructions in Bundles, we still need to process them as such, i.e.
| evaluate all instructions issued in the same cycle together.
\************************************************************************/

/*
    Our strategy is to first process all customGate's in a bundle, storing the
    relevant information in bundleInfo. Then, when all work for a bundle has
    been collected, we generate code in bundleFinish

    - bundleStart():
    clear bundleInfo, which maintains the work that needs to be performed for bundle

    - custom_instruction():
    collect instruction (FKA as gate) information in bundleInfo

    - bundleFinish():
    generate code for bundle from information collected in bundleInfo (which
    may be empty if no custom gates are present in bundle)
*/

// bundleStart: see 'strategy' above
void Codegen::bundleStart(const Str &cmnt) {
    // create ragged 'matrix' of BundleInfo with proper vector size per instrument
    bundleInfo.clear();
    BundleInfo empty;
    for (UInt instrIdx = 0; instrIdx < settings.getInstrumentsSize(); instrIdx++) {
        const Settings::InstrumentControl ic = settings.getInstrumentControl(instrIdx);
        bundleInfo.emplace_back(
            ic.controlModeGroupCnt,     // one BundleInfo per group in the control mode selected for instrument
            empty                       // empty BundleInfo
        );
    }

    // generate source code comments
    comment(cmnt);
    dp.comment(cmnt, options->verbose);      // FIXME: comment is not fully appropriate, but at least allows matching with .CODE section
}


Codegen::CodeGenMap Codegen::collectCodeGenInfo(
    UInt startCycle,
    UInt durationInCycles
) {
    CodeGenMap codeGenMap;

    // iterate over instruments
    for (UInt instrIdx = 0; instrIdx < settings.getInstrumentsSize(); instrIdx++) {
        // get control info from instrument settings
        const Settings::InstrumentControl ic = settings.getInstrumentControl(instrIdx);
        if (ic.ii.slot >= MAX_SLOTS) {
            QL_JSON_ERROR(
                "illegal slot " << ic.ii.slot
                << " on instrument '" << ic.ii.instrumentName
            );
        }

        /************************************************************************\
        | collect code generation info from all groups within one instrument
        \************************************************************************/

        // FIXME: the term 'group' is used in a diffused way: 1) index of signal vectors, 2) controlModeGroup

        CodeGenInfo codeGenInfo = {false};

        // remind information needed for code generation
        codeGenInfo.instrumentName = ic.ii.instrumentName;
        codeGenInfo.slot = ic.ii.slot;

        // now collect code generation info from all groups of instrument
        UInt nrGroups = bundleInfo[instrIdx].size();
        for (UInt group = 0; group < nrGroups; group++) {
            const BundleInfo &bi = bundleInfo[instrIdx][group];           // shorthand

            // handle output
            if (!bi.signalValue.empty()) {                         // signal defined, i.e.: we need to output something
                // compute maximum duration over all groups
                if (bi.durationInCycles > codeGenInfo.instrMaxDurationInCycles) {
                    codeGenInfo.instrMaxDurationInCycles = bi.durationInCycles;
                }

                CalcGroupDigOut gdo = calcGroupDigOut(instrIdx, group, nrGroups, ic, bi.staticCodewordOverride);
                codeGenInfo.digOut |= gdo.groupDigOut;
                comment(gdo.comment);

                // conditional gates
                // store condition and groupDigOut in condMap, if all groups are unconditional we use old scheme, otherwise
                // datapath is configured to generate proper digital output
                if (bi.condition == ConditionType::ALWAYS || ic.ii.forceCondGatesOn) {
                    // nothing to do, just use digOut
                } else {    // other conditions, including cond_never
                    // remind mapping for setting PL
                    codeGenInfo.condGateMap.emplace(group, CondGateInfo{bi.condition, bi.cond_operands, gdo.groupDigOut});
                }

                vcd.bundleFinishGroup(startCycle, bi.durationInCycles, gdo.groupDigOut, bi.signalValue, instrIdx, group);

                codeGenInfo.instrHasOutput = true;
            } // if(signal defined)


            // handle readout (i.e. when necessary, create feedbackMap entry
            // NB: we allow for instruments that only perform the input side of readout, without signal generation by the
            // same instrument.
            // FIXME: also generate VCD

            if (bi.isMeasFeedback) {
                UInt resultBit = Settings::getResultBit(ic, group);

#if 0    // FIXME: partly redundant, but partly useful
                // get our qubit
                const Json qubits = json_get<const Json>(*ic.ii.instrument, "qubits", ic.ii.instrumentName);   // NB: json_get<const Json&> unavailable
                UInt qubitGroupCnt = qubits.size();                                  // NB: JSON key qubits is a 'matrix' of [groups*qubits]
                if (group >= qubitGroupCnt) {    // FIXME: also tested in settings_cc::findSignalInfoForQubit
                    QL_JSON_ERROR("group " << group << " not defined in '" << ic.ii.instrumentName << "/qubits'");
                }
                const Json qubitsOfGroup = qubits[group];
                if (qubitsOfGroup.size() != 1) {    // FIXME: not tested elsewhere
                    QL_JSON_ERROR("group " << group << " of '" << ic.ii.instrumentName << "/qubits' should define 1 qubit, not " << qubitsOfGroup.size());
                }
                Int qubit = qubitsOfGroup[0].get<Int>();
                if (bi.readoutQubit != qubit) {              // this instrument group handles requested qubit. FIXME: inherently true
                    QL_ICE("inconsistency FIXME");
                };
#endif
                // get classic operand
                UInt breg_operand;
                if (bi.breg_operands.empty()) {
                    breg_operand = bi.operands[0];                    // implicit classic bit for qubit
                    QL_IOUT("Using implicit bit " << breg_operand << " for qubit " << bi.operands[0]);
                } else {
                    breg_operand = bi.breg_operands[0];
                    QL_IOUT("Using explicit bit " << breg_operand << " for qubit " << bi.operands[0]);
                }

                // allocate SM bit for classic operand
                UInt smBit = dp.allocateSmBit(breg_operand, instrIdx);

                // remind mapping of bit -> smBit for setting MUX
                codeGenInfo.feedbackMap.emplace(group, FeedbackInfo{smBit, resultBit, bi});
            }
        } // for(group)
        codeGenMap.set(instrIdx) = codeGenInfo;
     } // for(instrIdx)
     return codeGenMap;
}


// bundleFinish: see 'strategy' above
void Codegen::bundleFinish(
    UInt startCycle,
    UInt durationInCycles,
    Bool isLastBundle
) {
    // collect info for all instruments
    CodeGenMap codeGenMap = collectCodeGenInfo(startCycle, durationInCycles);

    // compute stuff requiring overview over all instruments:
    // FIXME: add
    // - DSM used, for seq_inv_sm

    // determine whether bundle has any feedback
    Bool bundleHasFeedback = false;
    for (const auto &codeGenInfo : codeGenMap) {
        if (!codeGenInfo.second.feedbackMap.empty()) {
            bundleHasFeedback = true;
            // FIXME: calc min and max SM address used
            //  unsigned int smAddr = datapath_cc::getMuxSmAddr(feedbackMap);
        }
    }

    // turn code generation info collected above into actual code
    for (UInt instrIdx = 0; instrIdx < settings.getInstrumentsSize(); instrIdx++) {
        CodeGenInfo codeGenInfo = codeGenMap.at(instrIdx);

        if (isLastBundle && instrIdx == 0) {
            comment(QL_SS2S(" # last bundle of kernel, will pad outputs to match durations"));
        }

        // generate code for instrument output
        if (codeGenInfo.instrHasOutput) {
            emitOutput(
                codeGenInfo.condGateMap,
                codeGenInfo.digOut,
                codeGenInfo.instrMaxDurationInCycles,
                instrIdx,
                startCycle,
                codeGenInfo.slot,
                codeGenInfo.instrumentName
            );
        } else {    // !instrHasOutput
            // nothing to do, we delay emitting till a slot is used or kernel finishes (i.e. isLastBundle just below)
        }

        if (bundleHasFeedback) {
            emitFeedback(
                codeGenInfo.feedbackMap,
                instrIdx,
                startCycle,
                codeGenInfo.slot,
                codeGenInfo.instrumentName
            );
        }

        // for last bundle, pad end of bundle to align durations
        if (isLastBundle) {
            emitPadToCycle(
                instrIdx, startCycle + durationInCycles,
                codeGenInfo.slot,
                codeGenInfo.instrumentName
            );        // FIXME: use instrMaxDurationInCycles and/or check consistency
        }

        vcd.bundleFinish(
            startCycle,
            codeGenInfo.digOut,
            codeGenInfo.instrMaxDurationInCycles,
            instrIdx
        );    // FIXME: conditional gates, etc
    } // for(instrIdx)

    comment("");    // blank line to separate bundles
}

/************************************************************************\
| Quantum instructions
\************************************************************************/

// custom_instruction: single/two/N qubit gate, including readout, see 'strategy' above
// translates 'gate' representation to 'waveform' representation (BundleInfo) and maps qubits to instruments & group.
// Does not deal with the control mode and digital interface of the instrument.
void Codegen::custom_instruction(const ir::CustomInstruction &custom) {
        // Handle the condition. NB: the 'condition' field exists for all conditional_instruction sub types,
        // but we only handle it for custom_instruction
        tInstructionCondition instrCond = decode_condition(operandContext, custom.condition);

        Operands ops;

        // Handle the template operands for the instruction_type we got. Note that these are empty if that is a 'root'
        // InstructionType, and only contains data if it is one of the specializations (see ir.gen.h)

// FIXME: these are operands that match a specialized instruction definition, e.g. "cz q0,q10"
// FIXME: these are not handled below, so things fail if we have such definitions
//cQASM "cz q[0],q[10]" with JSON "cz q0,q10" results in:
//[OPENQL] /Volumes/Data/shared/GIT/OpenQL/src/ql/arch/cc/pass/gen/vq1asm/detail/backend.cc:152 custom instruction: name=cz
//[OPENQL] /Volumes/Data/shared/GIT/OpenQL/src/ql/arch/cc/pass/gen/vq1asm/detail/codegen.cc:814 found template_operands: JSON = {"cc":{"signal":[],"static_codeword_override":[0]},"duration":40}
//[OPENQL] /Volumes/Data/shared/GIT/OpenQL/src/ql/arch/cc/pass/gen/vq1asm/detail/codegen.cc:798 template operand: q[0]
//[OPENQL] /Volumes/Data/shared/GIT/OpenQL/src/ql/arch/cc/pass/gen/vq1asm/detail/codegen.cc:798 template operand: q[10]
//
//cQASM "cz q[0],q[10]" with JSON "cz q0,q9" results in:
//[OPENQL] /Volumes/Data/shared/GIT/OpenQL/src/ql/arch/cc/pass/gen/vq1asm/detail/backend.cc:152 custom instruction: name=cz
//[OPENQL] /Volumes/Data/shared/GIT/OpenQL/src/ql/arch/cc/pass/gen/vq1asm/detail/codegen.cc:814 found template_operands: JSON = {"cc":{"signal":[],"static_codeword_override":[0]},"duration":40}
//[OPENQL] /Volumes/Data/shared/GIT/OpenQL/src/ql/arch/cc/pass/gen/vq1asm/detail/codegen.cc:798 template operand: q[0]
//[OPENQL] /Volumes/Data/shared/GIT/OpenQL/src/ql/arch/cc/pass/gen/vq1asm/detail/codegen.cc:809 operand: q[10]
//
//cQASM "cz q[0],q[10]" with JSON "cz q1,q10" results in:
//[OPENQL] /Volumes/Data/shared/GIT/OpenQL/src/ql/arch/cc/pass/gen/vq1asm/detail/backend.cc:152 custom instruction: name=cz
//[OPENQL] /Volumes/Data/shared/GIT/OpenQL/src/ql/arch/cc/pass/gen/vq1asm/detail/codegen.cc:829 operand: q[0]
//[OPENQL] /Volumes/Data/shared/GIT/OpenQL/src/ql/arch/cc/pass/gen/vq1asm/detail/codegen.cc:829 operand: q[10]

        // FIXME: check for existing decompositions (which should have been performed already by an upstream pass)

        if(!custom.instruction_type->template_operands.empty()) {
            QL_DOUT("found template_operands: JSON = " << custom.instruction_type->data.data );
            QL_INPUT_ERROR("CC backend cannot yet handle specialized instructions");
        }

        for (const auto &ob : custom.instruction_type->template_operands) {
            QL_DOUT("template operand: " + ir::describe(ob));
            try {
                ops.append(operandContext, ob);
            } catch (utils::Exception &e) {
                e.add_context("name=" + custom.instruction_type->name + ", qubits=" + ops.qubits.to_string());
                throw;
            }
        }

        // Handle the 'plain' operands for custom instructions.
        for (utils::UInt i = 0; i < custom.operands.size(); i++) {
            QL_DOUT("operand: " + ir::describe(custom.operands[i]));
            try {
                ops.append(operandContext, custom.operands[i]);
            } catch (utils::Exception &e) {
                e.add_context(
                    "name=" + custom.instruction_type->name
                    + ", qubits=" + ops.qubits.to_string()
                    + ", operand=" + std::to_string(i)
                    );
                throw;
            }
        }

#if 0   // org: processes org.has_integer/ops.integer
        kernel->gate(
            custom.instruction_type->name, ops.qubits, ops.cregs,
            0, ops.angle, ops.bregs
        );
        if (ops.has_integer) {
            CHECK_COMPAT(
                kernel->gates.size() == first_gate_index + 1,
                "gate with integer operand cannot be ad-hoc decomposed"
            );
            kernel->gates.back()->int_operand = ops.integer;
        }
#endif

        // FIXME: if we have a cz with operands for which no decomposition exists, we'll end up with:
        // RuntimeError: JSON error: in pass VQ1Asm, phase main: in block 'repeatUntilSuccess': in for loop body: instruction not found: 'cz'
        // This provides little insight, and why do we get upto here anyway? See above: template_operands


        // some shorthand for parameter fields
        const Str iname = custom.instruction_type->name;
        UInt durationInCycles = custom.instruction_type->duration;

#if 0   // FIXME: test for angle parameter
        if(ops.angle != 0.0) {
            QL_DOUT("iname=" << iname << ", angle=" << angle);
        }
#endif

        vcd.customGate(iname, ops.qubits, custom.cycle, durationInCycles);

        // generate comment
        Bool isReadout = settings.isReadout(*custom.instruction_type);        //  determine whether this is a readout instruction
        // FIXME: does this make a lot of sense, only triggers for "_dist_dsm"
        if (isReadout) {
            comment(Str(" # READOUT: '") + ir::describe(custom) + "'");
        } else { // handle all other instruction types than "readout"
            // generate comment. NB: we don't have a particular limit for the number of operands
            comment(Str(" # gate '") + ir::describe(custom) + "'");
        }

        // find signal vector definition for instruction
        const Json &instruction = custom.instruction_type->data.data;
        Settings::SignalDef sd = settings.findSignalDefinition(instruction, iname);

        // scatter signals defined for instruction (e.g. several operands and/or types) to instruments & groups
        for (UInt s = 0; s < sd.signal.size(); s++) {
            CalcSignalValue csv = calcSignalValue(sd, s, ops.qubits, iname);

            // store signal value, checking for conflicts
            BundleInfo &bi = bundleInfo[csv.si.instrIdx][csv.si.group];         // shorthand
            if (!csv.signalValueString.empty()) {                               // empty implies no signal
                if (bi.signalValue.empty()) {                                   // signal not yet used
                    bi.signalValue = csv.signalValueString;
#if OPT_SUPPORT_STATIC_CODEWORDS
                    // FIXME: this does not only provide support, but findStaticCodewordOverride() currently actually requires static codewords
                    bi.staticCodewordOverride = Settings::findStaticCodewordOverride(instruction, csv.operandIdx, iname); // NB: function return -1 means 'no override'
#endif
                } else if (bi.signalValue == csv.signalValueString) {           // signal unchanged
                    // do nothing
                } else {
                    showCodeSoFar();
                    QL_USER_ERROR(
                        "Signal conflict on instrument='" << csv.si.ic.ii.instrumentName
                        << "', group=" << csv.si.group
                        << ", between '" << bi.signalValue
                        << "' and '" << csv.signalValueString << "'"
                    );  // FIXME: add offending instruction
                }
            }

            // store signal duration
            bi.durationInCycles = durationInCycles;

            // FIXME: assumes that group configuration for readout input matches that of output
            // store operands used for readout, actual work is postponed to bundleFinish()
            if (isReadout) {
                // FIXME: isReadout in itself does nothing, and doesn't occur in conf files: cleanup
                /*
                 * In the old IR, kernel->gate allows 3 types of measurement:
                 *         - no explicit result. Historically this implies either:
                 *             - no result, measurement results are often read offline from the readout device (mostly the raw values
                 *             instead of the binary result), without the control device ever taking notice of the value
                 *             - implicit bit result for qubit, e.g. for the CC-light using conditional gates
                 *         - creg result (old, no longer valid)
                 *             note that Creg's are managed through a class, whereas bregs are just numbers
                 *         - breg result (new)
                 *
                 *  In the new IR (or, better said, in the new way "prototype"s for instruction operands van be defined
                 *  using access modes as described in
                 *  https://openql.readthedocs.io/en/latest/gen/reference_configuration.html#instructions-section
                 *  it is not well possible to specify a measurement that returns its result in a different bit than
                 *  the default bit.
                 *  Since this poses no immediate problem, we only support measurements to the implicit default bit.
                 *
                 *  Also note that old_to_new.cc only uses the qubit operand, and the whole fact that any type of
                 *  operand could be specified to any gate is a quirk of the kernel.gate() functions of the API.
                 */

                // operand checks.
                // Note that if all instruction definitions have proper prototypes this would be guaranteed upstream.
                if (ops.qubits.size() != 1) {
                    QL_INPUT_ERROR(
                        "Readout instruction '" << ir::describe(custom)
                        << "' requires exactly 1 quantum operand, not " << ops.qubits.size()
                    );
                }
#if 0   // FIXME
                if (!ops.cregs.empty()) {
                    QL_INPUT_ERROR("Using Creg as measurement target is deprecated, use new breg_operands");
                }
                if (ops.bregs.size() > 1) {
                    QL_INPUT_ERROR(
                        "Readout instruction '" << ir::describe(custom)
                        << "' requires 0 or 1 bit operands, not " << ops.bregs.size()
                    );
                }
#endif

                // store operands
                // FIXME: this generates code to read the DIO interface and distribute the result, see "_dist_dsm"
                if (settings.getReadoutMode(*custom.instruction_type) == "feedback") {
                    bi.isMeasFeedback = true;
                    bi.operands = ops.qubits;
                    //bi.creg_operands = ops.cregs;    // NB: will be empty because of checks performed earlier
                    bi.breg_operands = ops.bregs;
                }
            }

            // store 'expression' for conditional gates
            // FIXME: change bi to use tInstructionCondition
            bi.condition = instrCond.cond_type;
            bi.cond_operands = instrCond.cond_operands;

            QL_DOUT("customGate(): iname='" << iname <<
                 "', duration=" << durationInCycles <<
                 " [cycles], instrIdx=" << csv.si.instrIdx <<
                 ", group=" << csv.si.group);

            // NB: code is generated in bundleFinish()
        }   // for(signal)
}

/************************************************************************\
| Structured control flow
\************************************************************************/

void Codegen::if_elif(const ir::ExpressionRef &condition, const Str &label, Int branch) {
    // finish previous branch
    if (branch>0) {
        emit("", "jmp", as_target(to_end(label)));
    }

    comment(
        "# IF_ELIF: "
        "condition = '" + ir::describe(condition) + "'"
        ", label = '" + label + "'"
    );

    if(branch > 0) {    // label not used if branch==0
        Str my_label = to_ifbranch(label, branch);
        emit(as_label(my_label));
    }

    Str jmp_label = to_ifbranch(label, branch+1);
    handle_expression(condition, jmp_label, "if.condition");
}


void Codegen::if_otherwise(const Str &label, Int branch) {
    comment(
        "# IF_OTHERWISE: "
        ", label = '" + label + "'"
    );

    Str my_label = to_ifbranch(label, branch);
    emit(as_label(my_label));
}


void Codegen::if_end(const Str &label) {
    comment(
        "# IF_END: "
        ", label = '" + label + "'"
    );

     emit(as_label(to_end(label)));
}


void Codegen::foreach_start(const ir::Reference &lhs, const ir::IntLiteral &frm, const Str &label) {
    check_int_literal(frm.value);

    comment(
        "# FOREACH_START: "
        "from = " + ir::describe(frm)
        + ", label = '" + label + "'"
    );

    auto reg = QL_SS2S("R" << creg2reg(lhs));
    emit("", "move", QL_SS2S(frm.value << "," << reg));
    // FIXME: if loop has no contents at all, register dependency is violated
    emit(as_label(to_start(label)));    // label for looping or 'continue'
}


void Codegen::foreach_end(const ir::Reference &lhs, const ir::IntLiteral &frm, const ir::IntLiteral &to, const Str &label) {
    check_int_literal(to.value);

    comment(
        "# FOREACH_END: "
        "from = " + ir::describe(frm)
        + ", to = " + ir::describe(to)
        + ", label = '" + label + "'"
    );

    auto reg = QL_SS2S("R" << creg2reg(lhs));

    if(to.value >= frm.value) {     // count up
        emit("", "add", QL_SS2S(reg << ",1," << reg));
        emit("", "nop");
        emit("", "jlt", QL_SS2S(reg << "," << to.value+1 << "," << as_target(to_start(label))), "# loop");
    } else {
        if(to.value == 0) {
            emit("", "loop", QL_SS2S(reg << "," << as_target(to_start(label))), "# loop");
        } else {
            emit("", "sub", QL_SS2S(reg << ",1," << reg));
            emit("", "nop");
            emit("", "jge", QL_SS2S(reg << "," << to.value << "," << as_target(to_start(label))), "# loop");
        }
    }

    emit(as_label(to_end(label)));    // label for loop end or 'break'
}


void Codegen::repeat(const Str &label) {
    comment(
        "# REPEAT: "
        ", label = '" + label + "'"
    );
    emit(as_label(to_start(label)));    // label for looping or 'continue'
}


void Codegen::until(const ir::ExpressionRef &condition, const Str &label) {
    comment(
        "# UNTIL: "
        "condition = '" + ir::describe(condition) + "'"
        ", label = '" + label + "'"
    );
    handle_expression(condition, to_end(label), "until.condition");
    emit("", "jmp", as_target(to_start(label)), "# loop");
    emit(as_label(to_end(label)));    // label for loop end or 'break'
}

// NB: also used for 'while' loops
void Codegen::for_start(utils::Maybe<ir::SetInstruction> &initialize, const ir::ExpressionRef &condition, const Str &label) {
    comment(
        "# LOOP_START: "
        + (!initialize.empty() ? "initialize = '"+ir::describe(initialize)+"', " : "")
        + "condition = '" + ir::describe(condition) + "'"
    );

    // for loop: initialize
    if (!initialize.empty()) {
        handle_set_instruction(*initialize, "for.initialize");
        emit("", "nop");    // register dependency between initialize and handle_expression (if those use the same register, which is likely)
    }

    emit(as_label(to_start(label)));    // label for looping or 'continue'
    handle_expression(condition, to_end(label), "for/while.condition");
}


void Codegen::for_end(utils::Maybe<ir::SetInstruction> &update, const Str &label) {
    comment(
        "# LOOP_END: "
        + (!update.empty() ? " update = '"+ir::describe(update)+"'" : "")
    );
    if (!update.empty()) {
        handle_set_instruction(*update, "for.update");
    }
    emit("", "jmp", as_target(to_start(label)), "# loop");
    emit(as_label(to_end(label)));    // label for loop end or 'break'
}


void Codegen::do_break(const Str &label) {
    emit("", "jmp", as_target(to_end(label)), "# break");
}


void Codegen::do_continue(const Str &label) {
    emit("", "jmp", as_target(to_start(label)), "# continue");
}


void Codegen::comment(const Str &c) {
    if (options->verbose) {
        emit(Str(2*depth, ' ') + c);  // indent by depth
    }
}

/************************************************************************\
| new IR expressions
\************************************************************************/

void Codegen::handle_set_instruction(const ir::SetInstruction &set, const Str &descr)
{
    QL_DOUT(descr << ": '" << ir::describe(set) << "'");
    do_handle_expression(set.rhs, set.lhs, "", descr);

}

void Codegen::handle_expression(const ir::ExpressionRef &expression, const Str &label_if_false, const Str &descr)
{
    QL_DOUT(descr << ": '" << ir::describe(expression) << "'");
    do_handle_expression(expression, One<ir::Expression>(), label_if_false, descr);
}


/************************************************************************\
|
| private functions
|
\************************************************************************/

/************************************************************************\
| Some helpers to ease nice assembly formatting
\************************************************************************/

// FIXME: assure space between fields!
// FIXME: make comment output depend on verboseCode

// FIXME: merge with next function
void Codegen::emit(const Str &labelOrComment, const Str &instr) {
    if (labelOrComment.empty()) {                           // no label
        codeSection << "                " << instr << std::endl;
    } else if (labelOrComment.length() < 16) {              // label fits before instr
        codeSection << std::setw(16) << labelOrComment << std::setw(16) << instr << std::endl;
    } else if (instr.empty()) {                             // no instr
        codeSection << labelOrComment << std::endl;
    } else {
        codeSection << labelOrComment << std::endl << "                " << instr << std::endl;
    }
}


// @param   labelOrSel      label must include trailing ":"
// @param   comment         must include leading "#"
void Codegen::emit(const Str &labelOrSel, const Str &instr, const Str &ops, const Str &comment) {
    codeSection << std::setw(16) << labelOrSel << std::setw(16) << instr << std::setw(36) << ops << comment << std::endl;
}

void Codegen::emit(Int slot, const Str &instr, const Str &ops, const Str &comment) {
    emit(QL_SS2S("[" << slot << "]"), instr, ops, comment);
}

/************************************************************************\
| helpers
\************************************************************************/

void Codegen::showCodeSoFar() {
    // provide context to help finding reason. FIXME: limit # lines
    QL_EOUT("Code so far:\n" << codeSection.str());
}


void Codegen::emitProgramStart(const Str &progName) {
    // emit program header
    codeSection << std::left;    // assumed by emit()
    codeSection << "# Program: '" << progName << "'" << std::endl;   // NB: put on top so it shows up in internal CC logging
    codeSection << "# CC_BACKEND_VERSION " << CC_BACKEND_VERSION_STRING << std::endl;
    codeSection << "# OPENQL_VERSION " << OPENQL_VERSION_STRING << std::endl;
    codeSection << "# Note:    generated by OpenQL Central Controller backend" << std::endl;
    codeSection << "#" << std::endl;

    emit(".CODE");   // start .CODE section

    // NB: new seq_bar semantics (firmware from 20191219 onwards)
    comment("# synchronous start and latency compensation");
    emit("",                "seq_bar",  "",                 "# synchronization, delay set externally through SET_SEQ_BAR_CNT");
    emit("",                "seq_out",  "0x00000000,1",     "# allows monitoring actual start time using trace unit");
    if (!options->run_once) {
        comment("# start of main loop that runs indefinitely");
        emit("__mainLoop:",     "",         "",                 "# ");    // FIXME: __mainLoop should be a forbidden kernel name
    }

    // initialize state
    emit("",                "seq_state","0",                "# clear Programmable Logic state");
}


void Codegen::emitProgramFinish() {
    comment("# finish program");
    if (options->run_once) {   // program runs once only
        emit("", "stop");
    } else {   // CC-light emulation: loop indefinitely
        // prevent real time pipeline emptying during jmp below (especially in conjunction with pragma/break
        emit("", "seq_wait", "1");

        // loop indefinitely
        emit("",      // no CCIO selector
             "jmp",
             "@__mainLoop",
             "# loop indefinitely");
    }

    emit(".END");   // end .CODE section
}


// generate code to input measurement results and distribute them via DSM
void Codegen::emitFeedback(
    const FeedbackMap &feedbackMap,
    UInt instrIdx,
    UInt startCycle,
    Int slot,
    const Str &instrumentName
) {
    if (startCycle > lastEndCycle[instrIdx]) {  // i.e. if(!instrHasOutput)
        emitPadToCycle(instrIdx, startCycle, slot, instrumentName);
    }

    // code generation for participating and non-participating instruments (NB: must take equal number of sequencer cycles)
    if (!feedbackMap.empty()) {    // this instrument performs readout for feedback now
        UInt mux = dp.getOrAssignMux(instrIdx, feedbackMap);
        dp.emitMux(mux, feedbackMap, instrIdx, slot);

        // emit code for slot input
        UInt sizeTag = Datapath::getSizeTag(feedbackMap.size());        // compute DSM transfer size tag (for 'seq_in_sm' instruction)
        UInt smAddr = Datapath::getMuxSmAddr(feedbackMap);
        emit(
            slot,
            "seq_in_sm",
            QL_SS2S("S" << smAddr << ","  << mux << "," << sizeTag),
            QL_SS2S("# cycle " << lastEndCycle[instrIdx] << "-" << lastEndCycle[instrIdx]+1 << ": feedback on '" << instrumentName+"'")
        );
        lastEndCycle[instrIdx]++;
    } else {    // this instrument does not perform readout for feedback now
        // emit code for non-participating instrument
        // FIXME: may invalidate DSM that ust arrived dependent on individual SEQBAR counts
        UInt smAddr = 0;
        UInt smTotalSize = 1;    // FIXME: inexact, but me must not invalidate memory that we will not write
        emit(
            slot,
            "seq_inv_sm",
            QL_SS2S("S" << smAddr << ","  << smTotalSize),
            QL_SS2S("# cycle " << lastEndCycle[instrIdx] << "-" << lastEndCycle[instrIdx]+1 << ": invalidate SM on '" << instrumentName+"'")
        );
        lastEndCycle[instrIdx]++;
    }
}


void Codegen::emitOutput(
        const CondGateMap &condGateMap,
        tDigital digOut,
        UInt instrMaxDurationInCycles,
        UInt instrIdx,
        UInt startCycle,
        Int slot,
        const Str &instrumentName
) {
    comment(QL_SS2S(
        "  # slot=" << slot
        << ", instrument='" << instrumentName << "'"
        << ": lastEndCycle=" << lastEndCycle[instrIdx]
        << ", startCycle=" << startCycle
        << ", instrMaxDurationInCycles=" << instrMaxDurationInCycles
    ));

    emitPadToCycle(instrIdx, startCycle, slot, instrumentName);

    // emit code for slot output
    if (condGateMap.empty()) {    // all groups unconditional
        emit(
            slot,
            "seq_out",
            QL_SS2S("0x" << std::hex << std::setfill('0') << std::setw(8) << digOut << std::dec << "," << instrMaxDurationInCycles),
            QL_SS2S("# cycle " << startCycle << "-" << startCycle + instrMaxDurationInCycles << ": code word/mask on '" << instrumentName + "'")
        );
    } else {    // at least one group conditional
        // configure datapath PL
        UInt pl = dp.getOrAssignPl(instrIdx, condGateMap);
        UInt smAddr = dp.emitPl(pl, condGateMap, instrIdx, slot);

        // emit code for conditional gate
        emit(
            slot,
            "seq_out_sm",
            QL_SS2S("S" << smAddr << "," << pl << "," << instrMaxDurationInCycles),
            QL_SS2S("# cycle " << startCycle << "-" << startCycle + instrMaxDurationInCycles << ": conditional code word/mask on '" << instrumentName << "'")
        );
    }

    // update lastEndCycle
    lastEndCycle[instrIdx] = startCycle + instrMaxDurationInCycles;
}


#if 0   // FIXME
void Codegen::emitPragma(
    const Json &pragma,
    Int pragmaSmBit,
    UInt instrIdx,
    UInt startCycle,
    Int slot,
    const Str &instrumentName
) {
    if (startCycle > lastEndCycle[instrIdx]) {    // i.e. if(!instrHasOutput)
        emitPadToCycle(instrIdx, startCycle, slot, instrumentName);
    }

    // FIXME: the only pragma possible is "break" for now
    Int pragmaBreakVal = json_get<Int>(pragma, "break", "pragma of unknown instruction");        // FIXME: we don't know which instruction we're dealing with, so better move
    UInt smAddr = pragmaSmBit / 32;    // 'seq_cl_sm' is addressable in 32 bit words
    UInt mask = 1ul << (pragmaSmBit % 32);
    std::string label = pragmaLoopLabel.back() + "_end";        // FIXME: must match label set in forEnd(), assumes we are actually inside a for loop

    // emit code for pragma "break". NB: code is identical for all instruments
    // FIXME: verify that instruction duration matches actual time
/*
    seq_cl_sm   S<address>          ; pass 32 bit SM-data to Q1 ...
    seq_wait    3                   ; prevent starvation of real time part during instructions below: 4 classic instructions + 1 branch
    move_sm     Ra                  ; ... and move to register
    nop                             ; register dependency Ra
    and         Ra,<mask>,Rb        ; mask depends on DSM bit location
    nop                             ; register dependency Rb
    jlt         Rb,1,@loop
*/
    emit(slot, "seq_cl_sm", QL_SS2S("S" << smAddr), QL_SS2S("# 'break if " << pragmaBreakVal << "' on '" << instrumentName << "'"));
    emit(slot, "seq_wait", "3");
    emit(slot, "move_sm", REG_TMP0, "");
    emit(slot, "nop");
    emit(slot, "and", QL_SS2S(REG_TMP0<< "," << mask << "," << REG_TMP1));    // results in '0' for 'bit==0' and 'mask' for 'bit==1'
    emit(slot, "nop");
    if (pragmaBreakVal == 0) {
        emit(slot, "jlt", QL_SS2S(REG_TMP1 << ",1,@" << label));
    } else {
        emit(slot, "jge", QL_SS2S(REG_TMP1 << ",1,@" << label));
    }
}
#endif


void Codegen::emitPadToCycle(UInt instrIdx, UInt startCycle, Int slot, const Str &instrumentName) {
    // compute prePadding: time to bridge to align timing
    Int prePadding = startCycle - lastEndCycle[instrIdx];
    if (prePadding < 0) {
        QL_EOUT("Inconsistency detected in bundle contents: printing code generated so far");
        showCodeSoFar();
        QL_INPUT_ERROR(
            "Inconsistency detected in bundle contents: time travel not yet possible in this version: prePadding=" << prePadding
            << ", startCycle=" << startCycle
            << ", lastEndCycle=" << lastEndCycle[instrIdx]
            << ", instrumentName='" << instrumentName << "'"
            << ", instrIdx=" << instrIdx
        );
    }

    if (prePadding > 0) {     // we need to align
        emit(
            slot,
            "seq_wait",
            QL_SS2S(prePadding),
            QL_SS2S("# cycle " << lastEndCycle[instrIdx] << "-" << startCycle << ": padding on '" << instrumentName+"'")
        );
    }

    // update lastEndCycle
    lastEndCycle[instrIdx] = startCycle;
}


// compute signalValueString, and some meta information, for sd[s] (i.e. one of the signals in the JSON definition of an instruction)
Codegen::CalcSignalValue Codegen::calcSignalValue(
    const Settings::SignalDef &sd,
    UInt s,
    const Vec<UInt> &operands,
    const Str &iname
) {
    CalcSignalValue ret;
    Str signalSPath = QL_SS2S(sd.path<<"["<<s<<"]");                   // for JSON error reporting

    /************************************************************************\
    | get signal properties, mapping operand index to qubit
    \************************************************************************/

    // get the operand index & qubit to work on
    ret.operandIdx = json_get<UInt>(sd.signal[s], "operand_idx", signalSPath);
    if (ret.operandIdx >= operands.size()) {
        QL_JSON_ERROR(
            "instruction '" << iname
            << "': JSON file defines operand_idx " << ret.operandIdx
            << ", but only " << operands.size()
            << " operands were provided (correct JSON, or provide enough operands)"
        ); // FIXME: add offending statement
    }
    UInt qubit = operands[ret.operandIdx];

    // get signal value
    const Json instructionSignalValue = json_get<const Json>(sd.signal[s], "value", signalSPath);   // NB: json_get<const Json&> unavailable
    Str sv = QL_SS2S(instructionSignalValue);   // serialize/stream instructionSignalValue into std::string

    // get instruction signal type (e.g. "mw", "flux", etc)
    // NB: instructionSignalType is different from "instruction/type" provided by find_instruction_type, although some identical strings are used). NB: that key is no longer used by the 'core' of OpenQL
    Str instructionSignalType = json_get<Str>(sd.signal[s], "type", signalSPath);

    /************************************************************************\
    | map signal type for qubit to instrument & group
    \************************************************************************/

    // find signalInfo, i.e. perform the mapping
    ret.si = settings.findSignalInfoForQubit(instructionSignalType, qubit);

    if (instructionSignalValue.empty()) {    // allow empty signal
        ret.signalValueString = "";
    } else {
        // verify signal dimensions
        UInt channelsPergroup = ret.si.ic.controlModeGroupSize;
        UInt size = instructionSignalValue.is_array() ? instructionSignalValue.size() : 1;  // For objects, size() returns number of keys
        if (size != channelsPergroup) {
//            QL_JSON_ERROR(
            QL_WOUT(    // FIXME: we're transitioning on the semantics of signalValue
                "signal dimension mismatch on instruction '" << iname
                << "' : control mode '" << ret.si.ic.refControlMode
                << "' requires " <<  channelsPergroup
                << " signals, but signal '" << signalSPath+"/value"
                << "' provides " << size
                << " (value='" << instructionSignalValue << "')"
            );
        }

        // expand macros
        sv = replace_all(sv, "\"", "");   // get rid of quotes
#if 1   // FIXME: deprecate?
        sv = replace_all(sv, "{gateName}", iname);
        sv = replace_all(sv, "{instrumentName}", ret.si.ic.ii.instrumentName);
        sv = replace_all(sv, "{instrumentGroup}", to_string(ret.si.group));
        // FIXME: allow using all qubits involved (in same signalType?, or refer to signal: qubitOfSignal[n]), e.g. qubit[0], qubit[1], qubit[2]
        sv = replace_all(sv, "{qubit}", to_string(qubit));
#endif
        ret.signalValueString = sv;

        // FIXME: note that the actual contents of the signalValue only become important when we'll do automatic codeword assignment and provide codewordTable to downstream software to assign waveforms to the codewords
    }

    comment(QL_SS2S(
        "  # slot=" << ret.si.ic.ii.slot
        << ", instrument='" << ret.si.ic.ii.instrumentName << "'"
        << ", group=" << ret.si.group
        << "': signalValue='" << ret.signalValueString << "'"
    ));

    return ret;
}


#if !OPT_SUPPORT_STATIC_CODEWORDS
Codeword codegen_cc::assignCodeword(const Str &instrumentName, Int instrIdx, Int group) {
    Codeword codeword;
    Str signalValue = bi->signalValue;

    if (QL_JSON_EXISTS(codewordTable, instrumentName) &&                        // instrument exists
                    codewordTable[instrumentName].size() > group) {         // group exists
        Bool cwFound = false;
        // try to find signalValue
        Json &myCodewordArray = codewordTable[instrumentName][group];
        for (codeword = 0; codeword < myCodewordArray.size() && !cwFound; codeword++) {   // NB: JSON find() doesn't work for arrays
            if (myCodewordArray[codeword] == signalValue) {
                QL_DOUT("signal value found at cw=" << codeword);
                cwFound = true;
            }
        }
        if (!cwFound) {
            Str msg = QL_SS2S("signal value '" << signalValue
                    << "' not found in group " << group
                    << ", which contains " << myCodewordArray);
            if (mapPreloaded) {
                QL_USER_ERROR("mismatch between preloaded 'backend_cc_map_input_file' and program requirements:" << msg)
            } else {
                QL_DOUT(msg);
                // NB: codeword already contains last used value + 1
                // FIXME: check that number is available
                myCodewordArray[codeword] = signalValue;                    // NB: structure created on demand
            }
        }
    } else {    // new instrument or group
        if (mapPreloaded) {
            QL_USER_ERROR("mismatch between preloaded 'backend_cc_map_input_file' and program requirements: instrument '"
                  << instrumentName << "', group "
                  << group
                  << " not present in file");
        } else {
            codeword = 1;
            codewordTable[instrumentName][group][0] = "";                   // code word 0 is empty
            codewordTable[instrumentName][group][codeword] = signalValue;   // NB: structure created on demand
        }
    }
    return codeword;
}
#endif

/************************************************************************\
| expression helpers
\************************************************************************/

Int Codegen::creg2reg(const ir::Reference &ref) {
    auto reg = operandContext.convert_creg_reference(ref);
    if(reg >= NUM_CREGS) {
        QL_INPUT_ERROR("register index " << reg << " exceeds maximum");
    }
    return reg;
};

// FIXME: recursion?
// FIXME: or pass SetInstruction or Expression depending on use
// FIXME: adopt structure of cQASM's cqasm-v1-functions-gen.cpp register_into used for constant propagation

/* Actually perform the code generation for an expression. Can be called to handle:
 * - the RHS of a SetInstruction, in which case parameter 'lhs' must be valid
 * - an Expression that acts as a condition for structured control, in which case parameter 'label_if_false' must contain
 * the label to jump to if the expression evaluates as false
 * The distinction between the two modes of operation is made based on the type of expression, either 'bit' or 'int',
 * which is possible because of the rather strict separation between these two types.
 *
 *
 * To understand how cQASM functions end up in the IR, please note that functions are handled during analyzing cQASM,
 * see 'AnalyzerHelper::analyze_function()'.
 *
 * A default set of functions that only handle constant arguments is provided by libqasm, see
 * 'register_into(resolver::FunctionTable &table)'. These functions add a constant node to the IR when called (and fail
 * if the arguments are not constant)
 *
 * Some of these are overridden by OpenQL to allow use of non-constant arguments. This is a 2 step process, where
 * 'convert_old_to_new(const compat::PlatformRef &old)' adds functions to the platform using 'add_function_type',
 * and 'ql::ir::cqasm:read()' then walks 'ir->platform->functions' and adds the functions using 'register_function()'.
 * These functions add a 'cqv::Function' node to the IR (even if the arguments are constant).
 */
void Codegen::do_handle_expression(
    const ir::ExpressionRef &expression,
    const ir::ExpressionRef &lhs,
    const Str &label_if_false,
    const Str &descr
) {
    // function global helpers
    auto breg2reg = [this](const ir::ExpressionRef &ref) {    // FIXME: makes no sense, and needs to go through DSM bit allocator
        auto reg = operandContext.convert_breg_reference(ref);
        if(reg >= NUM_BREGS) {
            QL_INPUT_ERROR("bit register index " << reg << " exceeds maximum");
        }
        return reg;
    };

    auto dest_reg = [this, lhs]() {
        return creg2reg(*lhs->as_reference());
    };

    // Convert integer/creg function_call.operands expression to Q1 instruction argument.
    auto op_str_int = [this](const ir::ExpressionRef &op) {
        if(op->as_reference()) {
            return QL_SS2S("R" << creg2reg(*op->as_reference()));
        } else if(auto ilit = op->as_int_literal()) {
            check_int_literal(*ilit);
            return QL_SS2S(ilit->value);
        } else {
            QL_ICE("Expected integer operand");
        }
    };

    // Convert bit/breg function_call.operands expression to FIXME: return type makes no sense
    auto op_str_bit = [breg2reg](const ir::ExpressionRef &op) {   // FIXME: misnomer op_str_bit
        if(op->as_reference()) {
            return breg2reg(op);
        } else if(op->as_int_literal()) {   // FIXME: do literals make sense here
            return op->as_int_literal()->value;
        } else {
            QL_ICE("Expected bit operand");
        }
    };

    // emit code for casting a bit value (i.e. DSM bit) to an integer (i.e. Q1 register)
    auto emit_bin_cast = [this, breg2reg](Any<ir::Expression> operands, Int expOpCnt) {
        if(operands.size() != expOpCnt) {
            QL_ICE("Expected " << expOpCnt << " bit operands, got " << operands.size());
        }

        // Compute DSM address and mask for operands.
        UInt smAddr = 0;
        UInt mask = 0;      // mask for used SM bits in 32 bit word transferred using move_sm
        for (Int i=0; i<operands.size(); i++) {
            auto &op = operands[i];

            Int breg;
            if(op->as_reference()) {
                breg = breg2reg(op);
            } else {
                QL_ICE("Expected bit operand, got '" << ir::describe(op) << "'");
            }

            // get SM bit for classic operand (allocated during readout)
            UInt smBit = dp.getSmBit(breg);

            // compute and check SM address
            UInt mySmAddr = smBit / 32;    // 'seq_cl_sm' is addressable in 32 bit words
            if(i==0) {
                smAddr = mySmAddr;
            } else {
                if(smAddr != mySmAddr) {
                    QL_USER_ERROR("Cannot access DSM address " << smAddr << " and " << mySmAddr << " in single transfer");
                    // NB: we could setup several transfers
                }
            }

            // update mask of used bits
            mask |= 1ul << (smBit % 32);
        }

        // FIXME: verify that instruction duration matches actual time. We don't have a matching instruction for the break, but do take up quantum time
        /*
            seq_cl_sm   S<address>          ; pass 32 bit SM-data to Q1 ...
            seq_wait    3                   ; prevent starvation of real time part during instructions below: 4 classic instructions + 1 branch
            move_sm     Ra                  ; ... and move to register
            nop                             ; register dependency Ra

            and         Ra,<mask>,Rb        ; mask depends on DSM bit location
            nop                             ; register dependency Rb
            jlt         Rb,1,@loop
        */
        emit("", "seq_cl_sm", QL_SS2S("S" << smAddr));
        emit("", "seq_wait", "3");
        emit("", "move_sm", REG_TMP0);
        emit("", "nop");
        return mask;

#if 0
        // FIXME: move to caller:
        emit("", "and", QL_SS2S(REG_TMP0 << "," << mask << "," << REG_TMP1));    // results in '0' for 'bit==0' and 'mask' for 'bit==1'
        emit("", "nop");
#endif
#if 0
        if (pragmaBreakVal == 0) {
            emit("", "jlt", QL_SS2S(REG_TMP1 << ",1,@" << label));
        } else {
            emit("", "jge", QL_SS2S(REG_TMP1 << ",1,@" << label));
        }
#endif
    };
    // ----------- end of global helpers -------------


    try {
        if(!lhs.empty()) {
            comment(QL_SS2S("# Expression '" << descr << "': " << ir::describe(lhs) << " = " << ir::describe(expression)));
        } else {
#if 0   // FIXME: redundant information, also provided by structured control flow comments
            comment(QL_SS2S("# Expression '" << descr << "': " << ir::describe(expression)));
#endif
        }

        if (auto ilit = expression->as_int_literal()) {
            check_int_literal(*ilit);
            emit(
                "",
                "move",
                QL_SS2S(ilit->value << ",R" << dest_reg())
                , "# " + ir::describe(expression)
            );
#if 0 // FIXME
        } else if (expression->as_bit_literal()) {
#endif
        } else if (expression->as_reference()) {
            if(operandContext.is_creg_reference(expression)) {
                auto reg = creg2reg(*expression->as_reference());
                emit(
                    "",
                    "move",
                    QL_SS2S("R" << reg << ",R" << dest_reg())  // FIXME: use op_str_int?
                    , "# " + ir::describe(expression)
                );
            } else {
                // convert ir::Expression to utils::Any<ir::Expression>
                utils::Any<ir::Expression> anyExpression;
                anyExpression.add(expression);

                UInt mask = emit_bin_cast(anyExpression, 1);
                // FIXME: assign to LHS. Can we even write 'creg[0] = breg[0]' without a cast?
                emit("", "and", QL_SS2S(REG_TMP0 << "," << mask << "," << REG_TMP1));    // results in '0' for 'bit==0' and 'mask' for 'bit==1'
                emit("", "nop");
//                emit("", "jlt", QL_SS2S(REG_TMP1 << ",1,@" << label_if_false), "# " + ir::describe(expression));
            }
        } else if (auto fn = expression->as_function_call()) {
            // function call helpers
            enum Profile {
                LR,     // int Literal, Reference
                RL,
                RR
            };
            auto get_profile = [](Any<ir::Expression> operands) {
                CHECK_COMPAT(
                    operands.size() == 2,
                    "expected 2 operands"
                );
                if(operands[0]->as_int_literal() && operands[1]->as_reference()) {
                    return LR;
                } else if(operands[0]->as_reference() && operands[1]->as_int_literal()) {
                    return RL;
                } else if(operands[0]->as_reference() && operands[1]->as_reference()) {
                    return RR;
                } else if(operands[0]->as_int_literal() && operands[1]->as_int_literal()) {
                    QL_INPUT_ERROR("cannot currently handle functions on two literal paremeters");
                } else if(operands[0]->as_function_call()) {
                    QL_INPUT_ERROR("cannot handle function call within function call '" << ir::describe(operands[0]) << "'");
                    // FIXME: etc, also handle "creg(0)=creg(0)+1+1" or "1 < i+3"
                } else if(operands[1]->as_function_call()) {
                    QL_INPUT_ERROR("cannot handle function call within function call '" << ir::describe(operands[1]) << "'");
                } else {
                    QL_INPUT_ERROR("cannot handle parameter combination '" << ir::describe(operands[0]) << "' , '" << ir::describe(operands[1]) << "'");
                    // NB: includes both parameters being int_literal, which we may handle in the future by a separate pass
                }
            };
            auto emit_mnem2args = [this, op_str_int, fn, expression](const Str &mnem, Int arg0, Int arg1, const Str &target=REG_TMP0) {
                emit(
                    "",
                    mnem,
                    QL_SS2S(
                        op_str_int(fn->operands[arg0])
                        << "," << op_str_int(fn->operands[arg1])
                        << "," << target
                    )
                    , "# " + ir::describe(expression)
                );
            };
            // ----------- end of function call helpers -------------

            utils::Str operation;

            // handle cast
            if (fn->function_type->name == "int") {
                CHECK_COMPAT(
                    fn->operands.size() == 1 &&
                    fn->operands[0]->as_function_call(),
                    "'int()' cast target must be a function"
                );
                fn = fn->operands[0]->as_function_call();   // FIXME: step into. Shouldn't we recurse to allow e.g. casting a breg??

            // int arithmetic, 1 operand
            } else if (fn->function_type->name == "operator~") {
                operation = "not";
                emit(
                    "",
                    operation,
                    QL_SS2S(
                        op_str_int(fn->operands[0])
                        << ",R" << dest_reg()
                    )
                    , "# " + ir::describe(expression)
                );

            // bit arithmetic, 1 operand
            } else if (fn->function_type->name == "operator!") {
                operation = "not";
                UInt mask = emit_bin_cast(fn->operands, 1);

                emit("", "and", QL_SS2S(REG_TMP0 << "," << mask << "," << REG_TMP1));    // results in '0' for 'bit==0' and 'mask' for 'bit==1'
                emit("", "nop");
                emit("", "jlt", QL_SS2S(REG_TMP1 << ",1,@" << label_if_false), "# " + ir::describe(expression));
            }

            // int arithmetic, 2 operands
            if (operation.empty()) {    // check group only if nothing found yet
                if (fn->function_type->name == "operator+") {
                    operation = "add";
                } else if (fn->function_type->name == "operator-") {
                    operation = "sub";
                } else if (fn->function_type->name == "operator&") {
                    operation = "and";
                } else if (fn->function_type->name == "operator|") {
                    operation = "or";
                } else if (fn->function_type->name == "operator^") {
                    operation = "xor";
                }
                if (!operation.empty()) {
                    switch (get_profile(fn->operands)) {
                        case RL:    // fall through
                        case RR:    emit_mnem2args(operation, 0, 1, QL_SS2S("R"<<dest_reg())); break;
                        case LR:    emit_mnem2args(operation, 1, 0, QL_SS2S("R"<<dest_reg())); break;   // reverse operands to match Q1 instruction set
                            if (operation == "sub") {
                                // FIXME: correct for changed op order
                            }
                    }
                }
            }

            // bit arithmetic, 2 operands
            if(operation.empty()) {
                if (fn->function_type->name == "operator&&") {
                    operation = "FIXME";
                } else if (fn->function_type->name == "operator||") {
                    operation = "FIXME";
                } else if (fn->function_type->name == "operator^^") {
                    operation = "FIXME";
                }
                if (!operation.empty()) {
                    UInt mask = emit_bin_cast(fn->operands, 2);
                    // FIXME:
                    emit("", "and", QL_SS2S(REG_TMP0 << "," << mask << "," << REG_TMP1));    // results in '0' for 'bit==0' and 'mask' for 'bit==1'
                    emit("", "nop");
                    emit("", "jlt", QL_SS2S(REG_TMP1 << ",1,@" << label_if_false), "# " + ir::describe(expression));
                }
            }

            // relop, group 1
            if(operation.empty()) {
                if (fn->function_type->name == "operator==") {
                    operation = "jge";  // note that we need to invert the operation, because we jump on the condition being false
                } else if (fn->function_type->name == "operator!=") {
                    operation = "jlt";
                }
                if(!operation.empty()) {
                    switch (get_profile(fn->operands)) {
                        case RL:    // fall through
                        case RR:    emit_mnem2args("xor", 0, 1); break;
                        case LR:    emit_mnem2args("xor", 1, 0); break;   // reverse operands to match Q1 instruction set
                        // FIXME: optimization possible if Literal==0
                    }
                    emit("", "nop");    // register dependency
                    emit("", operation, Str(REG_TMP0)+",1,@"+label_if_false, "# skip next part if condition is false");
                }
            }

            // relop, group 2
            if(operation.empty()) {
                if (fn->function_type->name == "operator>=") {
                    operation = ">=";   // NB: actual contents unused here
                    switch (get_profile(fn->operands)) {
                        case RL:    // fall through
                        case RR:    emit_mnem2args("jge", 0, 1, as_target(label_if_false)); break;
                        case LR:    emit_mnem2args("jlt", 1, 0, as_target(label_if_false)); break;   // reverse operands (and instruction) to match Q1 instruction set
                    }
                } else if (fn->function_type->name == "operator<") {
                    operation = "<";
                    switch (get_profile(fn->operands)) {
                        case RL:    // fall through
                        case RR:    emit_mnem2args("jlt", 0, 1, as_target(label_if_false)); break;
                        case LR:    emit_mnem2args("jge", 1, 0, as_target(label_if_false)); break;   // reverse operands (and instruction) to match Q1 instruction set
                    }
                } else if (fn->function_type->name == "operator>") {
                    operation = ">";
                    switch (get_profile(fn->operands)) {
                        case RL:
                            check_int_literal(*fn->operands[1]->as_int_literal(), 0, 1);
                            emit(
                                "",
                                "jge",
                                QL_SS2S(
                                    op_str_int(fn->operands[0]) << ","
                                    << fn->operands[1]->as_int_literal()->value + 1    // increment literal since we lack 'jgt'
                                    << ",@"+label_if_false
                                ),
                                "# skip next part if condition is false"
                            );
                            break;
                        case RR:
                            emit(
                                "",
                                "add",
                                QL_SS2S(
                                    "1,"
                                    << op_str_int(fn->operands[1])
                                    << "," << REG_TMP0
                                )
                            );                      // increment arg1
                                emit("", "nop");    // register dependency
                            emit(
                                "",
                                "jge",
                                QL_SS2S(
                                    op_str_int(fn->operands[0])
                                    << "," << REG_TMP0
                                    << ",@"+label_if_false
                                ),
                                "# skip next part if condition is false"
                            );
                            break;
                        case LR:
                            check_int_literal(*fn->operands[0]->as_int_literal(), 1, 0);
                            emit(
                                "",
                                "jlt",                              // reverse instruction
                                QL_SS2S(
                                    op_str_int(fn->operands[1])     // reverse operands
                                    << fn->operands[0]->as_int_literal()->value - 1    // DECrement literal since we lack 'jle'
                                    << ",@"+label_if_false
                                ),
                                "# skip next part if condition is false"
                            );
                            break;
                    }
                } else if (fn->function_type->name == "operator<=") {
                    operation = "<=";
                    QL_ICE("FIXME: '<=' not yet implemented");
                }
                if(!operation.empty()) {
                    // NB: all work already done above
                }
            }

            if(operation.empty()) {
                // NB: if we arrive here, there's an inconsistency between the functions registered in
                // 'ql::ir::cqasm:read()' (see comment at beginning of this function) and our decoding here.
                QL_ICE(
                    "function '" << fn->function_type->name << "' not supported by CC backend, but it should be"
                );
            }
        }
    }
    catch (utils::Exception &e) {
        e.add_context("in expression '" + ir::describe(expression) + "'", true);
        throw;
    }

}


} // namespace detail
} // namespace vq1asm
} // namespace gen
} // namespace pass
} // namespace cc
} // namespace arch
} // namespace ql
