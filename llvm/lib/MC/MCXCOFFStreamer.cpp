//===- lib/MC/MCXCOFFStreamer.cpp - XCOFF Object Output -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file assembles .s files and emits XCOFF .o object files.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCXCOFFStreamer.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSymbolXCOFF.h"
#include "llvm/Support/TargetRegistry.h"

using namespace llvm;

MCXCOFFStreamer::MCXCOFFStreamer(MCContext &Context,
                                 std::unique_ptr<MCAsmBackend> MAB,
                                 std::unique_ptr<MCObjectWriter> OW,
                                 std::unique_ptr<MCCodeEmitter> Emitter)
    : MCObjectStreamer(Context, std::move(MAB), std::move(OW),
                       std::move(Emitter)) {}

bool MCXCOFFStreamer::EmitSymbolAttribute(MCSymbol *Symbol,
                                          MCSymbolAttr Attribute) {
  report_fatal_error("Symbol attributes not implemented for XCOFF.");
}

void MCXCOFFStreamer::EmitCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                                       unsigned ByteAlignment) {
  getAssembler().registerSymbol(*Symbol);
  Symbol->setExternal(cast<MCSymbolXCOFF>(Symbol)->getStorageClass() !=
                      XCOFF::C_HIDEXT);
  Symbol->setCommon(Size, ByteAlignment);

  // Need to add this symbol to the current Fragment which will belong to the
  // containing CSECT.
  auto *F = dyn_cast_or_null<MCDataFragment>(getCurrentFragment());
  assert(F && "Expected a valid section with a fragment set.");
  Symbol->setFragment(F);

  // Emit the alignment and storage for the variable to the section.
  EmitValueToAlignment(ByteAlignment);
  EmitZeros(Size);
}

void MCXCOFFStreamer::EmitZerofill(MCSection *Section, MCSymbol *Symbol,
                                   uint64_t Size, unsigned ByteAlignment,
                                   SMLoc Loc) {
  report_fatal_error("Zero fill not implemented for XCOFF.");
}

void MCXCOFFStreamer::EmitInstToData(const MCInst &Inst,
                                     const MCSubtargetInfo &) {
  report_fatal_error("Instruction emission not implemented for XCOFF.");
}

MCStreamer *llvm::createXCOFFStreamer(MCContext &Context,
                                      std::unique_ptr<MCAsmBackend> &&MAB,
                                      std::unique_ptr<MCObjectWriter> &&OW,
                                      std::unique_ptr<MCCodeEmitter> &&CE,
                                      bool RelaxAll) {
  MCXCOFFStreamer *S = new MCXCOFFStreamer(Context, std::move(MAB),
                                           std::move(OW), std::move(CE));
  if (RelaxAll)
    S->getAssembler().setRelaxAll(true);
  return S;
}

void MCXCOFFStreamer::EmitXCOFFLocalCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                                            unsigned ByteAlign) {
  report_fatal_error("Emission of local commons not implemented yet.");
}
