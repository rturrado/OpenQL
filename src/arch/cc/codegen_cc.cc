/**
 * @file    codegen_cc.cc
 * @date    201810xx
 * @author  Wouter Vlothuizen (wouter.vlothuizen@tno.nl)
 * @brief   code generator backend for the Central Controller
 * @note    here we don't check whether the sequence of calling code generator
 *          functions is correct
 */

// constants:
#define CC_BACKEND_VERSION_STRING       "0.2.6"

#define UNUSED_COP  -1  // unused classic operand


#include "codegen_cc.h"
#include "eqasm_backend_cc.h"

#include <version.h>
#include <options.h>

/************************************************************************\
| Static functions processing JSON
\************************************************************************/

#if OPT_SUPPORT_STATIC_CODEWORDS
static int findStaticCodewordOverride(const json &instruction, size_t operandIdx, const std::string &iname)
{
    // look for optional codeword override
    int staticCodewordOverride = -1;    // -1 means unused
    if(JSON_EXISTS(instruction["cc"], "static_codeword_override")) {    // optional keyword
 #if OPT_STATIC_CODEWORDS_ARRAYS
        const json &override = instruction["cc"]["static_codeword_override"];
        if(override.is_array()) {
            if(override.size() > operandIdx) {
                staticCodewordOverride = override[operandIdx];
            } else {
                FATAL("Array size of static_codeword_override for instruction '" << iname << "' insufficient");
            }
        } else if (operandIdx==0) {     // NB: JSON '"static_codeword_override": [3]' gives **scalar** result
            staticCodewordOverride = override;
        } else {
            FATAL("Key static_codeword_override for instruction '" << iname
                  << "' should be an array (found '" << override << "' in '" << instruction << "')");
        }
 #else
        staticCodewordOverride = instruction["cc"]["static_codeword_override"];
 #endif
        DOUT("Found static_codeword_override=" << staticCodewordOverride <<
             " for instruction '" << iname << "'");
    }
 #if 1 // FIXME: require override
    if(staticCodewordOverride < 0) {
        FATAL("No static codeword defined for instruction '" << iname <<
            "' (we currently require it because automatic assignment is disabled)");
    }
 #endif
    return staticCodewordOverride;
}
#endif

/************************************************************************\
| Generic
\************************************************************************/

void codegen_cc::init(const ql::quantum_platform &platform)
{
    // NB: a new eqasm_backend_cc is instantiated per call to compile, and
    // as a result also a codegen_cc, so we don't need to cleanup
    this->platform = &platform;
    settings.load_backend_settings(platform);

    // optionally preload codewordTable
    std::string map_input_file = ql::options::get("backend_cc_map_input_file");
    if(map_input_file != "") {
        DOUT("loading map_input_file='" << map_input_file << "'");
        json map = ql::load_json(map_input_file);
        codewordTable = map["codeword_table"];      // FIXME: use json_get
        mapPreloaded = true;
    }
}

std::string codegen_cc::getProgram()
{
    return codeSection.str();
    // FIXME: append datapathSection
}

std::string codegen_cc::getMap()
{
    json map;

    map["note"] = "generated by OpenQL CC backend version " CC_BACKEND_VERSION_STRING;
    map["codeword_table"] = codewordTable;
#if OPT_FEEDBACK
    map["inputLut_table"] = inputLutTable;
#endif
    return SS2S(std::setw(4) << map << std::endl);
}


/************************************************************************\
| Compile support
\************************************************************************/

void codegen_cc::program_start(const std::string &progName)
{
    // emit program header
    codeSection << std::left;    // assumed by emit()
    codeSection << "# Program: '" << progName << "'" << std::endl;   // NB: put on top so it shows up in internal CC logging
    codeSection << "# CC_BACKEND_VERSION " << CC_BACKEND_VERSION_STRING << std::endl;
    codeSection << "# OPENQL_VERSION " << OPENQL_VERSION_STRING << std::endl;
    codeSection << "# Note:    generated by OpenQL Central Controller backend" << std::endl;
    codeSection << "#" << std::endl;

    emitProgramStart();
#if OPT_VCD_OUTPUT
    vcd.programStart(platform->qubit_number, platform->cycle_time, MAX_GROUPS, settings);
#endif
}


void codegen_cc::program_finish(const std::string &progName)
{
#if OPT_RUN_ONCE   // program runs once only
    emit("", "stop");
#else   // CC-light emulation: loop indefinitely
    emit("",      // no CCIO selector
         "jmp",
         "@mainLoop",
         "# loop indefinitely");
#endif
#if OPT_FEEDBACK
    emit(".END");   // end .CODE section
#endif

#if OPT_VCD_OUTPUT
    vcd.programFinish(progName);
#endif
}

void codegen_cc::kernel_start()
{
    ql::utils::zero(lastEndCycle);       // FIXME: actually, bundle.startCycle starts counting at 1
}

void codegen_cc::kernel_finish(const std::string &kernelName, size_t durationInCycles)
{
#if OPT_VCD_OUTPUT
    vcd.kernelFinish(kernelName, durationInCycles);
#endif
}

/*
    Our strategy is to first process all custom_gate's in a bundle, storing the
    relevant information in bundleInfo. Then, when all work for a bundle has
    been collected, we generate code in bundle_finish

    - bundle_start():
    clear bundleInfo, which maintains the work that needs to be performed for bundle

    - custom_gate():
    collect gate information in bundleInfo

    - bundle_finish():
    generate code for bundle from information collected in bundleInfo (which
    may be empty if no custom gates are present in bundle)
*/

// bundle_start: see 'strategy' above
void codegen_cc::bundle_start(const std::string &cmnt)
{
    unsigned int slotsUsed = settings.getInstrumentsSize();   // FIXME: assuming all instruments use a slot
    bundleInfo.assign(slotsUsed, std::vector<tBundleInfo>(MAX_GROUPS, {"", 0, -1}));

    comment(cmnt);
}

// bundle_finish: see 'strategy' above
// FIXME: split into smaller parts
void codegen_cc::bundle_finish(size_t startCycle, size_t durationInCycles, bool isLastBundle)
{
    if(isLastBundle) {
        comment(SS2S(" # last bundle of kernel, will pad outputs to match durations"));
    }

    // iterate over instruments
    for(size_t instrIdx=0; instrIdx<settings.getInstrumentsSize(); instrIdx++) {
        const settings_cc::tInstrumentInfo ii = settings.getInstrumentInfo(instrIdx);

        // collect code generation info from all groups within one instrument
        // FIXME: the term 'group' is used in a diffused way: 1) index of signal vectors, 2) controlModeGroup
        bool isInstrUsed = false;
        uint32_t digOut = 0;                                                // the digital output value sent over the instrument interface
        uint32_t digIn = 0;
        uint32_t maxDurationInCycles = 0;                                   // maximum duration over groups that are used, one instrument

        size_t nrGroups = bundleInfo[instrIdx].size();       // FIXME: always MAX_GROUPS:
        // FIXME: 20200924 error shows 32 iso 4: "E       TypeError: Error : instrument 'mw_0' uses 32 groups, but control mode 'awg8-mw-direct-iq' defines 2 trigger bits in 'trigger_bits' (must be 1 or #groups)"
        for(size_t group=0; group<nrGroups; group++) {                      // iterate over groups used within instrument
            tBundleInfo *gi = &bundleInfo[instrIdx][group];                 // shorthand
            if(gi->signalValue != "") {                                     // signal defined, i.e.: we need to output something
                isInstrUsed = true;

                // determine control mode group FIXME: more explanation
                int controlModeGroup = -1;
                if(ii.nrControlBitsGroups == 0) {
                    FATAL("'control_bits' not defined in 'control_modes/" << ii.refControlMode <<"'");
#if OPT_VECTOR_MODE
                } else if(ii.nrControlBitsGroups == 1) {                    // vector mode: group addresses channel within vector
                    controlModeGroup = 0;
#endif
                } else if(group < ii.nrControlBitsGroups) {                 // normal mode: group selects control group
                    controlModeGroup = group;
                } else {
                    FATAL("instrument '" << ii.instrumentName
                          << "' uses " << nrGroups
                          << " groups, but control mode '" << ii.refControlMode
                          << "' only defines " << ii.nrControlBitsGroups
                          << " groups in 'control_bits'");
                }

                // get control bits
                const json groupControlBits = ii.controlMode["control_bits"][controlModeGroup];    // NB: tests above guarantee existence
                DOUT("instrumentName=" << ii.instrumentName
                     << ", slot=" << ii.slot
                     << ", control mode group=" << controlModeGroup
                     << ", group control bits: " << groupControlBits);
                size_t nrGroupControlBits = groupControlBits.size();


                // calculate digital output for group
                uint32_t groupDigOut = 0;           // codeword/mask fragment for this group
                if(nrGroupControlBits == 1) {       // single bit, implying this is a mask (not code word)
                    groupDigOut |= 1<<(int)groupControlBits[0];     // NB: we assume the mask is active high, which is correct for VSM and UHF-QC
                    // FIXME: check controlModeGroup vs group
                } else {                // > 1 bit, implying code word
#if OPT_VECTOR_MODE
                    //  allow single code word for vector of groups. FIXME: requires looking at all sd.signal before assigning code word
                    if(group != controlModeGroup) {
                        // FIXME: unfinished work on vector mode
                    }
#endif

                    // find or assign code word
                    uint32_t codeword = 0;
                    bool codewordOverriden = false;
#if OPT_SUPPORT_STATIC_CODEWORDS    // FIXME: this does not only provide support, but actually requires static codewords
                    int staticCodewordOverride = gi->staticCodewordOverride;
                    codeword = staticCodewordOverride;
                    codewordOverriden = true;
#else
                    codeword = assignCodeword(ii.instrumentName, instrIdx, group);
#endif

                    // convert codeword to digOut
                    for(size_t idx=0; idx<nrGroupControlBits; idx++) {
                        int codeWordBit = nrGroupControlBits-1-idx;    // NB: groupControlBits defines MSB..LSB
                        if(codeword & (1<<codeWordBit)) groupDigOut |= 1<<(int)groupControlBits[idx];
                    }

                    comment(SS2S("  # slot=" << ii.slot
                            << ", instrument='" << ii.instrumentName << "'"
                            << ", group=" << group
                            << ": codeword=" << codeword
                            << std::string(codewordOverriden ? " (static override)" : "")
                            << ": groupDigOut=0x" << std::hex << std::setfill('0') << std::setw(8) << groupDigOut
                            ));
                }

                digOut |= groupDigOut;


                // add trigger to digOut
                size_t nrTriggerBits = ii.controlMode["trigger_bits"].size();
                if(nrTriggerBits == 0) {                                    // no trigger
                    // do nothing
                } else if(nrTriggerBits == 1) {                             // single trigger for all groups
                    digOut |= 1 << (int)ii.controlMode["trigger_bits"][0];
                } else if(nrTriggerBits == nrGroups) {                      // trigger per group
                    digOut |= 1 << (int)ii.controlMode["trigger_bits"][group];
                } else {
                    FATAL("instrument '" << ii.instrumentName
                          << "' uses " << nrGroups
                          << " groups, but control mode '" << ii.refControlMode
                          << "' defines " << nrTriggerBits
                          << " trigger bits in 'trigger_bits' (must be 1 or #groups)");
                } // FIXME: e.g. HDAWG does not support > 1 trigger bit. dual-QWG required 2 trigger bits

                // compute slot duration
                unsigned int durationInCycles = platform->time_to_cycles(gi->durationInNs);
                if(durationInCycles > maxDurationInCycles) maxDurationInCycles = durationInCycles;

#if OPT_VCD_OUTPUT
                vcd.bundleFinishGroup(startCycle, gi->durationInNs, groupDigOut, gi->signalValue, instrIdx, group);
#endif
            } // if(signal defined)

            // handle readout
            // NB: we allow for readout without signal generation by the same instrument, which might be needed in the future
            // FIXME: test for gi->readoutCop >= 0
            // FIXME: check consistency between measure instruction and result_bits
            // FIXME: also generate VCD
            if(JSON_EXISTS(ii.controlMode, "result_bits")) {  // this instrument mode produces results (i.e. it is a measurement device)
                const json &resultBits = ii.controlMode["result_bits"][group];
                size_t nrResultBits = resultBits.size();
                if(nrResultBits == 1) {                     // single bit
                    digIn |= 1<<(int)resultBits[0];         // NB: we assume the result is active high, which is correct for UHF-QC

#if OPT_FEEDBACK   // FIXME: WIP on measurement
                    // we need: input bit, cop?, qubit, SM bit
                    // FIXME: save gi->readoutCop in inputLutTable
                    if(JSON_EXISTS(inputLutTable, ii.instrumentName) &&                    // instrument exists
                                    inputLutTable[ii.instrumentName].size() > group) {     // group exists
                    } else {    // new instrument or group
                        codeword = 1;
                        inputLutTable[ii.instrumentName][group][0] = "";                   // code word 0 is empty
                        inputLutTable[ii.instrumentName][group][codeword] = signalValue;   // NB: structure created on demand
                    }
#endif
                } else {    // NB: nrResultBits==0 will not arrive at this point
                    std::string controlModeName = ii.controlMode;                      // convert to string
                    FATAL("JSON key '" << controlModeName << "/result_bits' must have 1 bit per group");
                }
            }
        } // for(group)


        // generate code for instrument
        if(isInstrUsed) {
            comment(SS2S("  # slot=" << ii.slot
                    << ", instrument='" << ii.instrumentName << "'"
                    << ": lastEndCycle=" << lastEndCycle[instrIdx]
                    << ", startCycle=" << startCycle
                    << ", maxDurationInCycles=" << maxDurationInCycles
                    ));

            padToCycle(lastEndCycle[instrIdx], startCycle, ii.slot, ii.instrumentName);

            // emit code for slot
            emit(SS2S("[" << ii.slot << "]").c_str(),      // CCIO selector
                 "seq_out",
                 SS2S("0x" << std::hex << std::setfill('0') << std::setw(8) << digOut << std::dec <<
                      "," << maxDurationInCycles),
                 SS2S("# cycle " << startCycle << "-" << startCycle+maxDurationInCycles << ": code word/mask on '" << ii.instrumentName+"'").c_str());

            // update lastEndCycle
            lastEndCycle[instrIdx] = startCycle + maxDurationInCycles;

#if OPT_FEEDBACK
            if(digIn) { // FIXME
                comment(SS2S("# digIn=" << digIn));
                // get qop
                // get cop
                // get/assign LUT
                // seq_in_sm
            }
#endif
        } else {    // !isInstrUsed
            // nothing to do, we delay emitting till a slot is used or kernel finishes (i.e. isLastBundle just below)
        }

        // for last bundle, pad end of bundle to align durations
        if(isLastBundle) {
            padToCycle(lastEndCycle[instrIdx], startCycle+durationInCycles, ii.slot, ii.instrumentName);
        }

#if OPT_VCD_OUTPUT
        vcd.bundleFinish(startCycle, digOut, maxDurationInCycles, instrIdx);
#endif
    } // for(instrIdx)

    comment("");    // blank line to separate bundles
}

/************************************************************************\
| Quantum instructions
\************************************************************************/

// custom_gate: single/two/N qubit gate, including readout, see 'strategy' above
void codegen_cc::custom_gate(
        const std::string &iname,
        const std::vector<size_t> &qops,
        const std::vector<size_t> &cops,
        double angle, size_t startCycle, size_t durationInNs)
{
#if 0   // FIXME: test for angle parameter
    if(angle != 0.0) {
        DOUT("iname=" << iname << ", angle=" << angle);
    }
#endif

    /*  determine whether this is a readout instruction
        NB: we only use the instruction_type "readout" and don't care about the rest
        because the terms "mw" and "flux" don't fully cover gate functionality. It
        would be nice if custom gates could mimic ql::gate_type_t
    */
    bool isReadout = "readout" == platform->find_instruction_type(iname);

    // generate comment (also performs some checks)
    if(isReadout) {
        if(cops.size() == 0) {
            /*  NB: existing code uses empty cops, i.e. no explicit classical register.
                On the one hand this historically seems to imply assignment to an
                implicit 'register' in the CC-light that can be used for conditional
                gates.
                On the other hand, measurement results can also be read from the
                readout device without the control device ever taking notice of the
                value
            */
            // FIXME: define meaning: no classical target, or implied target (classical register matching qubit)
            comment(SS2S(" # READOUT: " << iname << "(q" << qops[0] << ")"));
        } else if(cops.size() == 1) {
            comment(SS2S(" # READOUT: " << iname << "(c" << cops[0] << ",q" << qops[0] << ")"));
        } else {
            FATAL("Readout instruction requires 0 or 1 classical operands, not " << cops.size());
        }
    } else { // handle all other instruction types than "readout"
        // generate comment. NB: we don't have a particular limit for the number of operands
        std::stringstream cmnt;
        cmnt << " # gate '" << iname << " ";    // FIXME: make QASM compatible, use library function?
        for(size_t i=0; i<qops.size(); i++) {
            cmnt << qops[i];
            if(i<qops.size()-1) cmnt << ",";
        }
        cmnt << "'";
        comment(cmnt.str());
    }

#if OPT_VCD_OUTPUT
    vcd.customgate(iname, qops, startCycle, durationInNs);
#endif

    // find instruction (gate definition)
    const json &instruction = platform->find_instruction(iname);
    // find signal vector definition for instruction
    settings_cc::tSignalDef sd = settings.findSignalDefinition(instruction, iname);

    // iterate over signal vector defined for instruction
    for(size_t s=0; s<sd.signal.size(); s++) {
        // compute signalValueString, and some meta information
        std::string signalValueString;
        unsigned int operandIdx;
        settings_cc::tSignalInfo si;
        settings_cc::tInstrumentInfo ii;
        {   // limit scope
            std::string signalSPath = SS2S(sd.path<<"["<<s<<"]");        // for JSON error reporting

            // get the operand index & qubit to work on
            operandIdx = json_get<unsigned int>(sd.signal[s], "operand_idx", signalSPath);
            if(operandIdx >= qops.size()) {
                FATAL("Error in JSON definition of instruction '" << iname <<
                      "': illegal operand number " << operandIdx <<
                      "' exceeds expected maximum of " << qops.size()-1)
            }
            unsigned int qubit = qops[operandIdx];

            // get signalInfo via signal type (e.g. "mw", "flux", etc. NB: this is different from
            // that provided by find_instruction_type, although some identical strings are used)
            std::string instructionSignalType = json_get<std::string>(sd.signal[s], "type", signalSPath);
            si = settings.findSignalInfoForQubit(instructionSignalType, qubit);

            // get instrument and CC slot
            ii = settings.getInstrumentInfo(si.instrIdx);
            // FIXME: computes more then we need here

            // get signal value and expand macros
            // FIXME: note that the actual contents of the signalValue only become important when we'll do automatic codeword assignment and
            // provide codewordTable to downstream software to assign waveforms to the codewords
            const json instructionSignalValue = json_get<const json>(sd.signal[s], "value", signalSPath);   // NB: json_get<const json&> unavailable
            signalValueString = SS2S(instructionSignalValue);   // serialize instructionSignalValue into std::string
            ql::utils::replace(signalValueString, std::string("\""), std::string(""));   // get rid of quotes
            ql::utils::replace(signalValueString, std::string("{gateName}"), iname);
            ql::utils::replace(signalValueString, std::string("{instrumentName}"), ii.instrumentName);
            ql::utils::replace(signalValueString, std::string("{instrumentGroup}"), std::to_string(si.group));
            // FIXME: allow using all qubits involved (in same signalType?, or refer to signal: qubitOfSignal[n]), e.g. qubit[0], qubit[1], qubit[2]
            ql::utils::replace(signalValueString, std::string("{qubit}"), std::to_string(qubit));

            comment(SS2S("  # slot=" << ii.slot
                    << ", instrument='" << ii.instrumentName << "'"
                    << ", group=" << si.group
                    << "': signalValue='" << signalValueString << "'"
                    ));
        }


        // store signal value, checking for conflicts
        tBundleInfo *gi = &bundleInfo[si.instrIdx][si.group];               // shorthand
        if(gi->signalValue == "") {                                         // signal not yet used
            gi->signalValue = signalValueString;
#if OPT_SUPPORT_STATIC_CODEWORDS
            gi->staticCodewordOverride = findStaticCodewordOverride(instruction, operandIdx, iname); // NB: -1 means unused
#endif
        } else if(gi->signalValue == signalValueString) {                   // signal unchanged
            // do nothing
        } else {
            EOUT("Code so far:\n" << codeSection.str());                    // provide context to help finding reason. FIXME: not great
            FATAL("Signal conflict on instrument='" << ii.instrumentName <<
                  "', group=" << si.group <<
                  ", between '" << gi->signalValue <<
                  "' and '" << signalValueString << "'");
        }

        // store signal duration
        gi->durationInNs = durationInNs;

#if OPT_FEEDBACK
        // store the classical operand used for readout
        if(isReadout) {
            int cop = cops.size()>0 ? cops[0] : UNUSED_COP;
            gi->readoutCop = cop;
        }

        // store expression for conditional gates
        // FIXME: implement
#endif

        DOUT("custom_gate(): iname='" << iname <<
             "', duration=" << durationInNs <<
             "[ns], si.instrIdx=" << si.instrIdx <<
             ", si.group=" << si.group);

        // NB: code is generated in bundle_finish()
    }   // for(signal)
}

void codegen_cc::nop_gate()
{
    comment("# NOP gate");
    FATAL("FIXME: NOP gate not implemented");
}

void codegen_cc::comment(const std::string &c)
{
    if(verboseCode) emit(c.c_str());
}

/************************************************************************\
| Classical operations on kernels
\************************************************************************/

void codegen_cc::if_start(size_t op0, const std::string &opName, size_t op1)
{
    comment(SS2S("# IF_START(R" << op0 << " " << opName << " R" << op1 << ")"));
    FATAL("FIXME: not implemented");
}

void codegen_cc::else_start(size_t op0, const std::string &opName, size_t op1)
{
    comment(SS2S("# ELSE_START(R" << op0 << " " << opName << " R" << op1 << ")"));
    FATAL("FIXME: not implemented");
}

void codegen_cc::for_start(const std::string &label, int iterations)
{
    comment(SS2S("# FOR_START(" << iterations << ")"));
    // FIXME: reserve register
    emit("", "move", SS2S(iterations << ",R62"), "# R62 is the 'for loop counter'");        // FIXME: fixed reg, no nested loops
    emit((label+":").c_str(), "", SS2S(""), "# ");        // just a label
}

void codegen_cc::for_end(const std::string &label)
{
    comment("# FOR_END");
    // FIXME: free register
    emit("", "loop", SS2S("R62,@" << label), "# R62 is the 'for loop counter'");        // FIXME: fixed reg, no nested loops
}

void codegen_cc::do_while_start(const std::string &label)
{
    comment("# DO_WHILE_START");
    emit((label+":").c_str(), "", SS2S(""), "# ");                              // NB: just a label
}

void codegen_cc::do_while_end(const std::string &label, size_t op0, const std::string &opName, size_t op1)
{
    comment(SS2S("# DO_WHILE_END(R" << op0 << " " << opName << " R" << op1 << ")"));
    emit("", "jmp", SS2S("@" << label), "# FIXME: we don't support conditions, just an endless loop'");        // FIXME: just endless loop
}

/************************************************************************\
| Classical arithmetic instructions
\************************************************************************/

#if 0   // FIXME: unused and incomplete
void codegen_cc::add() {
    FATAL("FIXME: not implemented");
}
#endif



/************************************************************************\
|
| private functions
|
\************************************************************************/

/************************************************************************\
| Some helpers to ease nice assembly formatting
\************************************************************************/

void codegen_cc::emit(const char *labelOrComment, const char *instr)
{
    if(!labelOrComment || strlen(labelOrComment)==0) {  // no label
        codeSection << "        " << instr << std::endl;
    } else if(strlen(labelOrComment)<8) {               // label fits before instr
        codeSection << std::setw(8) << labelOrComment << instr << std::endl;
    } else if(strlen(instr)==0) {                       // no instr
        codeSection << labelOrComment << std::endl;
    } else {
        codeSection << labelOrComment << std::endl << "        " << instr << std::endl;
    }
}

void codegen_cc::emit(const char *label, const char *instr, const std::string &qops, const char *comment)
{
    codeSection << std::setw(16) << label << std::setw(16) << instr << std::setw(24) << qops << comment << std::endl;
}
// FIXME: assure space between fields!
// FIXME: also provide the above with std::string parameters

/************************************************************************\
| helpers
\************************************************************************/

void codegen_cc::emitProgramStart()
{
#if OPT_FEEDBACK
    emit(".CODE");   // start .CODE section
#endif

    comment("# synchronous start and latency compensation");
#if OPT_CALCULATE_LATENCIES    // fixed compensation based on instrument latencies
    std::map<int, int> slotLatencies;   // maps slot to latency

    // get latencies per slot, iterating over instruments
    for(size_t instrIdx=0; instrIdx<settings.getInstrumentsSize(); instrIdx++) {
        const json &instrument = settings.getInstrumentAtIdx(instrIdx);                  // NB: always exists
        std::string instrumentRef = instrument["ref_instrument_definition"];    // FIXME: use json_get
        int slot = instrument["controller"]["slot"];    // FIXME: assuming controller being cc, use json_get

        // find latency
        const json &id = findInstrumentDefinition(instrumentRef);
        int latency = id["latency"];    // FIXME: json_get
        DOUT("latency of '" << instrumentRef << "' in slot " << slot << " is " << latency);
        slotLatencies.insert(std::make_pair(slot, latency));
    }

    // find max latency
    int maxLatency = 0;
    for(auto it=slotLatencies.begin(); it!=slotLatencies.end(); it++) {
        int latency = it->second;
        if(latency > maxLatency) maxLatency = latency;
    }
    DOUT("maxLatency = " << maxLatency);

    // align latencies
    for(auto it=slotLatencies.begin(); it!=slotLatencies.end(); it++) {
        int slot = it->first;
        int latency = it->second;
        const int minDelay = 1;     // min value for seq_bar
        int delayInCycles = minDelay + platform->time_to_cycles(maxLatency-latency);

        emit(SS2S("[" << slot << "]").c_str(),      // CCIO selector
            "seq_bar",
            SS2S(delayInCycles),    // FIXME: old semantics
            SS2S("# latency compensation").c_str());    // FIXME: add instrumentName/instrumentRef/latency
    }
#else   // user settable delay via register
 #if OPT_OLD_SEQBAR_SEMANTICS  // original seq_bar semantics
        // FIXME: is 'seq_bar 1' safe in the sense that we will never get an empty queue?
        emit("",                "add",      "R63,1,R0",         "# R63 externally set by user, prevent 0 value which would wrap counter");
        emit("",                "seq_bar",  "20",               "# synchronization");
        emit("syncLoop:",       "seq_out",  "0x00000000,1",     "# 20 ns delay");
        emit("",                "loop",     "R0,@syncLoop",     "# ");
 #else  // new seq_bar semantics (firmware from 20191219 onwards)
        emit("",                "seq_bar",  "",                 "# synchronization, delay set externally through SET_SEQ_BAR_CNT");
 #endif

        emit("mainLoop:",       "",         "",                 "# ");

 #if OPT_FEEDBACK
        emit("",                "seq_state","0",                "# clear Programmable Logic state");
 #endif
#endif
}


void codegen_cc::padToCycle(size_t lastEndCycle, size_t startCycle, int slot, const std::string &instrumentName)
{
    // compute prePadding: time to bridge to align timing
    int prePadding = startCycle - lastEndCycle;
    if(prePadding < 0) {
        EOUT("Inconsistency detected in bundle contents: printing code generated so far");
        EOUT(codeSection.str());        // show what we made. FIXME: limit # lines
        FATAL("Inconsistency detected in bundle contents: time travel not yet possible in this version: prePadding=" << prePadding <<
              ", startCycle=" << startCycle <<
              ", lastEndCycle=" << lastEndCycle <<
              ", instrumentName='" << instrumentName << "'");
    }

    if(prePadding > 0) {     // we need to align
        emit(SS2S("[" << slot << "]").c_str(),      // CCIO selector
            "seq_out",
            SS2S("0x00000000," << prePadding),
            SS2S("# cycle " << lastEndCycle << "-" << startCycle << ": padding on '" << instrumentName+"'").c_str());
    }
}


#if !OPT_SUPPORT_STATIC_CODEWORDS
uint32_t codegen_cc::assignCodeword(const std::string &instrumentName, int instrIdx, int group)
{
    uint32_t codeword;
    std::string signalValue = gi->signalValue;

    if(JSON_EXISTS(codewordTable, instrumentName) &&                    // instrument exists
                    codewordTable[instrumentName].size() > group) {     // group exists
        bool cwFound = false;
        // try to find signalValue
        json &myCodewordArray = codewordTable[instrumentName][group];
        for(codeword=0; codeword<myCodewordArray.size() && !cwFound; codeword++) {   // NB: JSON find() doesn't work for arrays
            if(myCodewordArray[codeword] == signalValue) {
                DOUT("signal value found at cw=" << codeword);
                cwFound = true;
            }
        }
        if(!cwFound) {
            std::string msg = SS2S("signal value '" << signalValue
                    << "' not found in group " << group
                    << ", which contains " << myCodewordArray);
            if(mapPreloaded) {
                FATAL("mismatch between preloaded 'backend_cc_map_input_file' and program requirements:" << msg)
            } else {
                DOUT(msg);
                // NB: codeword already contains last used value + 1
                // FIXME: check that number is available
                myCodewordArray[codeword] = signalValue;                    // NB: structure created on demand
            }
        }
    } else {    // new instrument or group
        if(mapPreloaded) {
            FATAL("mismatch between preloaded 'backend_cc_map_input_file' and program requirements: instrument '"
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
