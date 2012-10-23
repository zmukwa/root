//===-- RuntimeDyldELF.cpp - Run-time dynamic linker for MC-JIT -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of ELF support for the MC-JIT runtime dynamic linker.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "dyld"
#include "RuntimeDyldELF.h"
#include "JITRegistrar.h"
#include "ObjectImageCommon.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/IntervalMap.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ExecutionEngine/ObjectImage.h"
#include "llvm/ExecutionEngine/ObjectBuffer.h"
#include "llvm/Support/ELF.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Object/ELF.h"
using namespace llvm;
using namespace llvm::object;

namespace {

template<support::endianness target_endianness, bool is64Bits>
class DyldELFObject : public ELFObjectFile<target_endianness, is64Bits> {
  LLVM_ELF_IMPORT_TYPES(target_endianness, is64Bits)

  typedef Elf_Shdr_Impl<target_endianness, is64Bits> Elf_Shdr;
  typedef Elf_Sym_Impl<target_endianness, is64Bits> Elf_Sym;
  typedef Elf_Rel_Impl<target_endianness, is64Bits, false> Elf_Rel;
  typedef Elf_Rel_Impl<target_endianness, is64Bits, true> Elf_Rela;

  typedef Elf_Ehdr_Impl<target_endianness, is64Bits> Elf_Ehdr;

  typedef typename ELFDataTypeTypedefHelper<
          target_endianness, is64Bits>::value_type addr_type;

public:
  DyldELFObject(MemoryBuffer *Wrapper, error_code &ec);

  void updateSectionAddress(const SectionRef &Sec, uint64_t Addr);
  void updateSymbolAddress(const SymbolRef &Sym, uint64_t Addr);

  // Methods for type inquiry through isa, cast and dyn_cast
  static inline bool classof(const Binary *v) {
    return (isa<ELFObjectFile<target_endianness, is64Bits> >(v)
            && classof(cast<ELFObjectFile<target_endianness, is64Bits> >(v)));
  }
  static inline bool classof(
      const ELFObjectFile<target_endianness, is64Bits> *v) {
    return v->isDyldType();
  }
  static inline bool classof(const DyldELFObject *v) {
    return true;
  }
};

template<support::endianness target_endianness, bool is64Bits>
class ELFObjectImage : public ObjectImageCommon {
  protected:
    DyldELFObject<target_endianness, is64Bits> *DyldObj;
    bool Registered;

  public:
    ELFObjectImage(ObjectBuffer *Input,
                   DyldELFObject<target_endianness, is64Bits> *Obj)
    : ObjectImageCommon(Input, Obj),
      DyldObj(Obj),
      Registered(false) {}

    virtual ~ELFObjectImage() {
      if (Registered)
        deregisterWithDebugger();
    }

    // Subclasses can override these methods to update the image with loaded
    // addresses for sections and common symbols
    virtual void updateSectionAddress(const SectionRef &Sec, uint64_t Addr)
    {
      DyldObj->updateSectionAddress(Sec, Addr);
    }

    virtual void updateSymbolAddress(const SymbolRef &Sym, uint64_t Addr)
    {
      DyldObj->updateSymbolAddress(Sym, Addr);
    }

    virtual void registerWithDebugger()
    {
      JITRegistrar::getGDBRegistrar().registerObject(*Buffer);
      Registered = true;
    }
    virtual void deregisterWithDebugger()
    {
      JITRegistrar::getGDBRegistrar().deregisterObject(*Buffer);
    }
};

// The MemoryBuffer passed into this constructor is just a wrapper around the
// actual memory.  Ultimately, the Binary parent class will take ownership of
// this MemoryBuffer object but not the underlying memory.
template<support::endianness target_endianness, bool is64Bits>
DyldELFObject<target_endianness, is64Bits>::DyldELFObject(MemoryBuffer *Wrapper,
                                                          error_code &ec)
  : ELFObjectFile<target_endianness, is64Bits>(Wrapper, ec) {
  this->isDyldELFObject = true;
}

template<support::endianness target_endianness, bool is64Bits>
void DyldELFObject<target_endianness, is64Bits>::updateSectionAddress(
                                                       const SectionRef &Sec,
                                                       uint64_t Addr) {
  DataRefImpl ShdrRef = Sec.getRawDataRefImpl();
  Elf_Shdr *shdr = const_cast<Elf_Shdr*>(
                          reinterpret_cast<const Elf_Shdr *>(ShdrRef.p));

  // This assumes the address passed in matches the target address bitness
  // The template-based type cast handles everything else.
  shdr->sh_addr = static_cast<addr_type>(Addr);
}

template<support::endianness target_endianness, bool is64Bits>
void DyldELFObject<target_endianness, is64Bits>::updateSymbolAddress(
                                                       const SymbolRef &SymRef,
                                                       uint64_t Addr) {

  Elf_Sym *sym = const_cast<Elf_Sym*>(
                                 ELFObjectFile<target_endianness, is64Bits>::
                                   getSymbol(SymRef.getRawDataRefImpl()));

  // This assumes the address passed in matches the target address bitness
  // The template-based type cast handles everything else.
  sym->st_value = static_cast<addr_type>(Addr);
}

} // namespace


namespace llvm {

ObjectImage *RuntimeDyldELF::createObjectImage(ObjectBuffer *Buffer) {
  if (Buffer->getBufferSize() < ELF::EI_NIDENT)
    llvm_unreachable("Unexpected ELF object size");
  std::pair<unsigned char, unsigned char> Ident = std::make_pair(
                         (uint8_t)Buffer->getBufferStart()[ELF::EI_CLASS],
                         (uint8_t)Buffer->getBufferStart()[ELF::EI_DATA]);
  error_code ec;

  if (Ident.first == ELF::ELFCLASS32 && Ident.second == ELF::ELFDATA2LSB) {
    DyldELFObject<support::little, false> *Obj =
           new DyldELFObject<support::little, false>(Buffer->getMemBuffer(), ec);
    return new ELFObjectImage<support::little, false>(Buffer, Obj);
  }
  else if (Ident.first == ELF::ELFCLASS32 && Ident.second == ELF::ELFDATA2MSB) {
    DyldELFObject<support::big, false> *Obj =
           new DyldELFObject<support::big, false>(Buffer->getMemBuffer(), ec);
    return new ELFObjectImage<support::big, false>(Buffer, Obj);
  }
  else if (Ident.first == ELF::ELFCLASS64 && Ident.second == ELF::ELFDATA2MSB) {
    DyldELFObject<support::big, true> *Obj =
           new DyldELFObject<support::big, true>(Buffer->getMemBuffer(), ec);
    return new ELFObjectImage<support::big, true>(Buffer, Obj);
  }
  else if (Ident.first == ELF::ELFCLASS64 && Ident.second == ELF::ELFDATA2LSB) {
    DyldELFObject<support::little, true> *Obj =
           new DyldELFObject<support::little, true>(Buffer->getMemBuffer(), ec);
    return new ELFObjectImage<support::little, true>(Buffer, Obj);
  }
  else
    llvm_unreachable("Unexpected ELF format");
}

RuntimeDyldELF::~RuntimeDyldELF() {
}

void RuntimeDyldELF::resolveX86_64Relocation(uint8_t *LocalAddress,
                                             uint64_t FinalAddress,
                                             uint64_t Value,
                                             uint32_t Type,
                                             int64_t Addend) {
  switch (Type) {
  default:
    llvm_unreachable("Relocation type not implemented yet!");
  break;
  case ELF::R_X86_64_64: {
    uint64_t *Target = (uint64_t*)(LocalAddress);
    *Target = Value + Addend;
    break;
  }
  case ELF::R_X86_64_32:
  case ELF::R_X86_64_32S: {
    Value += Addend;
    assert((Type == ELF::R_X86_64_32 && (Value <= UINT32_MAX)) ||
           (Type == ELF::R_X86_64_32S && 
             ((int64_t)Value <= INT32_MAX && (int64_t)Value >= INT32_MIN)));
    uint32_t TruncatedAddr = (Value & 0xFFFFFFFF);
    uint32_t *Target = reinterpret_cast<uint32_t*>(LocalAddress);
    *Target = TruncatedAddr;
    break;
  }
  case ELF::R_X86_64_PC32: {
    uint32_t *Placeholder = reinterpret_cast<uint32_t*>(LocalAddress);
    int64_t RealOffset = *Placeholder + Value + Addend - FinalAddress;
    assert(RealOffset <= INT32_MAX && RealOffset >= INT32_MIN);
    int32_t TruncOffset = (RealOffset & 0xFFFFFFFF);
    *Placeholder = TruncOffset;
    break;
  }
  }
}

void RuntimeDyldELF::resolveX86Relocation(uint8_t *LocalAddress,
                                          uint32_t FinalAddress,
                                          uint32_t Value,
                                          uint32_t Type,
                                          int32_t Addend) {
  switch (Type) {
  case ELF::R_386_32: {
    uint32_t *Target = (uint32_t*)(LocalAddress);
    uint32_t Placeholder = *Target;
    *Target = Placeholder + Value + Addend;
    break;
  }
  case ELF::R_386_PC32: {
    uint32_t *Placeholder = reinterpret_cast<uint32_t*>(LocalAddress);
    uint32_t RealOffset = *Placeholder + Value + Addend - FinalAddress;
    *Placeholder = RealOffset;
    break;
    }
    default:
      // There are other relocation types, but it appears these are the
      // only ones currently used by the LLVM ELF object writer
      llvm_unreachable("Relocation type not implemented yet!");
      break;
  }
}

void RuntimeDyldELF::resolveARMRelocation(uint8_t *LocalAddress,
                                          uint32_t FinalAddress,
                                          uint32_t Value,
                                          uint32_t Type,
                                          int32_t Addend) {
  // TODO: Add Thumb relocations.
  uint32_t* TargetPtr = (uint32_t*)LocalAddress;
  Value += Addend;

  DEBUG(dbgs() << "resolveARMRelocation, LocalAddress: " << LocalAddress
               << " FinalAddress: " << format("%p",FinalAddress)
               << " Value: " << format("%x",Value)
               << " Type: " << format("%x",Type)
               << " Addend: " << format("%x",Addend)
               << "\n");

  switch(Type) {
  default:
    llvm_unreachable("Not implemented relocation type!");

  // Just write 32bit value to relocation address
  case ELF::R_ARM_ABS32 :
    *TargetPtr = Value;
    break;

  // Write first 16 bit of 32 bit value to the mov instruction.
  // Last 4 bit should be shifted.
  case ELF::R_ARM_MOVW_ABS_NC :
    Value = Value & 0xFFFF;
    *TargetPtr |= Value & 0xFFF;
    *TargetPtr |= ((Value >> 12) & 0xF) << 16;
    break;

  // Write last 16 bit of 32 bit value to the mov instruction.
  // Last 4 bit should be shifted.
  case ELF::R_ARM_MOVT_ABS :
    Value = (Value >> 16) & 0xFFFF;
    *TargetPtr |= Value & 0xFFF;
    *TargetPtr |= ((Value >> 12) & 0xF) << 16;
    break;

  // Write 24 bit relative value to the branch instruction.
  case ELF::R_ARM_PC24 :    // Fall through.
  case ELF::R_ARM_CALL :    // Fall through.
  case ELF::R_ARM_JUMP24 :
    int32_t RelValue = static_cast<int32_t>(Value - FinalAddress - 8);
    RelValue = (RelValue & 0x03FFFFFC) >> 2;
    *TargetPtr &= 0xFF000000;
    *TargetPtr |= RelValue;
    break;
  }
}

void RuntimeDyldELF::resolveMIPSRelocation(uint8_t *LocalAddress,
                                           uint32_t FinalAddress,
                                           uint32_t Value,
                                           uint32_t Type,
                                           int32_t Addend) {
  uint32_t* TargetPtr = (uint32_t*)LocalAddress;
  Value += Addend;

  DEBUG(dbgs() << "resolveMipselocation, LocalAddress: " << LocalAddress
               << " FinalAddress: " << format("%p",FinalAddress)
               << " Value: " << format("%x",Value)
               << " Type: " << format("%x",Type)
               << " Addend: " << format("%x",Addend)
               << "\n");

  switch(Type) {
  default:
    llvm_unreachable("Not implemented relocation type!");
    break;
  case ELF::R_MIPS_32:
    *TargetPtr = Value + (*TargetPtr);
    break;
  case ELF::R_MIPS_26:
    *TargetPtr = ((*TargetPtr) & 0xfc000000) | (( Value & 0x0fffffff) >> 2);
    break;
  case ELF::R_MIPS_HI16:
    // Get the higher 16-bits. Also add 1 if bit 15 is 1.
    Value += ((*TargetPtr) & 0x0000ffff) << 16;
    *TargetPtr = ((*TargetPtr) & 0xffff0000) |
                 (((Value + 0x8000) >> 16) & 0xffff);
    break;
   case ELF::R_MIPS_LO16:
    Value += ((*TargetPtr) & 0x0000ffff);
    *TargetPtr = ((*TargetPtr) & 0xffff0000) | (Value & 0xffff);
    break;
   }
}

void RuntimeDyldELF::resolveRelocation(uint8_t *LocalAddress,
                                       uint64_t FinalAddress,
                                       uint64_t Value,
                                       uint32_t Type,
                                       int64_t Addend) {
  switch (Arch) {
  case Triple::x86_64:
    resolveX86_64Relocation(LocalAddress, FinalAddress, Value, Type, Addend);
    break;
  case Triple::x86:
    resolveX86Relocation(LocalAddress, (uint32_t)(FinalAddress & 0xffffffffL),
                         (uint32_t)(Value & 0xffffffffL), Type,
                         (uint32_t)(Addend & 0xffffffffL));
    break;
  case Triple::arm:    // Fall through.
  case Triple::thumb:
    resolveARMRelocation(LocalAddress, (uint32_t)(FinalAddress & 0xffffffffL),
                         (uint32_t)(Value & 0xffffffffL), Type,
                         (uint32_t)(Addend & 0xffffffffL));
    break;
  case Triple::mips:    // Fall through.
  case Triple::mipsel:
    resolveMIPSRelocation(LocalAddress, (uint32_t)(FinalAddress & 0xffffffffL),
                          (uint32_t)(Value & 0xffffffffL), Type,
                          (uint32_t)(Addend & 0xffffffffL));
    break;
  default: llvm_unreachable("Unsupported CPU type!");
  }
}

void RuntimeDyldELF::processRelocationRef(const ObjRelocationInfo &Rel,
                                          ObjectImage &Obj,
                                          ObjSectionToIDMap &ObjSectionToID,
                                          const SymbolTableMap &Symbols,
                                          StubMap &Stubs) {

  uint32_t RelType = (uint32_t)(Rel.Type & 0xffffffffL);
  intptr_t Addend = (intptr_t)Rel.AdditionalInfo;
  const SymbolRef &Symbol = Rel.Symbol;

  // Obtain the symbol name which is referenced in the relocation
  StringRef TargetName;
  Symbol.getName(TargetName);
  DEBUG(dbgs() << "\t\tRelType: " << RelType
               << " Addend: " << Addend
               << " TargetName: " << TargetName
               << "\n");
  RelocationValueRef Value;
  // First search for the symbol in the local symbol table
  SymbolTableMap::const_iterator lsi = Symbols.find(TargetName.data());
  if (lsi != Symbols.end()) {
    Value.SectionID = lsi->second.first;
    Value.Addend = lsi->second.second;
  } else {
    // Search for the symbol in the global symbol table
    SymbolTableMap::const_iterator gsi =
        GlobalSymbolTable.find(TargetName.data());
    if (gsi != GlobalSymbolTable.end()) {
      Value.SectionID = gsi->second.first;
      Value.Addend = gsi->second.second;
    } else {
      SymbolRef::Type SymType;
      Symbol.getType(SymType);
      switch (SymType) {
        case SymbolRef::ST_Debug: {
          // TODO: Now ELF SymbolRef::ST_Debug = STT_SECTION, it's not obviously
          // and can be changed by another developers. Maybe best way is add
          // a new symbol type ST_Section to SymbolRef and use it.
          section_iterator si(Obj.end_sections());
          Symbol.getSection(si);
          if (si == Obj.end_sections())
            llvm_unreachable("Symbol section not found, bad object file format!");
          DEBUG(dbgs() << "\t\tThis is section symbol\n");
          Value.SectionID = findOrEmitSection(Obj, (*si), true, ObjSectionToID);
          Value.Addend = Addend;
          break;
        }
        case SymbolRef::ST_Unknown: {
          Value.SymbolName = TargetName.data();
          Value.Addend = Addend;
          break;
        }
        default:
          llvm_unreachable("Unresolved symbol type!");
          break;
      }
    }
  }
  DEBUG(dbgs() << "\t\tRel.SectionID: " << Rel.SectionID
               << " Rel.Offset: " << Rel.Offset
               << "\n");
  if (Arch == Triple::arm &&
      (RelType == ELF::R_ARM_PC24 ||
       RelType == ELF::R_ARM_CALL ||
       RelType == ELF::R_ARM_JUMP24)) {
    // This is an ARM branch relocation, need to use a stub function.
    DEBUG(dbgs() << "\t\tThis is an ARM branch relocation.");
    SectionEntry &Section = Sections[Rel.SectionID];
    uint8_t *Target = Section.Address + Rel.Offset;

    //  Look up for existing stub.
    StubMap::const_iterator i = Stubs.find(Value);
    if (i != Stubs.end()) {
      resolveRelocation(Target, (uint64_t)Target, (uint64_t)Section.Address +
                        i->second, RelType, 0);
      DEBUG(dbgs() << " Stub function found\n");
    } else {
      // Create a new stub function.
      DEBUG(dbgs() << " Create a new stub function\n");
      Stubs[Value] = Section.StubOffset;
      uint8_t *StubTargetAddr = createStubFunction(Section.Address +
                                                   Section.StubOffset);
      RelocationEntry RE(Rel.SectionID, StubTargetAddr - Section.Address,
                         ELF::R_ARM_ABS32, Value.Addend);
      if (Value.SymbolName)
        addRelocationForSymbol(RE, Value.SymbolName);
      else
        addRelocationForSection(RE, Value.SectionID);

      resolveRelocation(Target, (uint64_t)Target, (uint64_t)Section.Address +
                        Section.StubOffset, RelType, 0);
      Section.StubOffset += getMaxStubSize();
    }
  } else if (Arch == Triple::mipsel && RelType == ELF::R_MIPS_26) {
    // This is an Mips branch relocation, need to use a stub function.
    DEBUG(dbgs() << "\t\tThis is a Mips branch relocation.");
    SectionEntry &Section = Sections[Rel.SectionID];
    uint8_t *Target = Section.Address + Rel.Offset;
    uint32_t *TargetAddress = (uint32_t *)Target;

    // Extract the addend from the instruction.
    uint32_t Addend = ((*TargetAddress) & 0x03ffffff) << 2;

    Value.Addend += Addend;

    //  Look up for existing stub.
    StubMap::const_iterator i = Stubs.find(Value);
    if (i != Stubs.end()) {
      resolveRelocation(Target, (uint64_t)Target,
                        (uint64_t)Section.Address +
                        i->second, RelType, 0);
      DEBUG(dbgs() << " Stub function found\n");
    } else {
      // Create a new stub function.
      DEBUG(dbgs() << " Create a new stub function\n");
      Stubs[Value] = Section.StubOffset;
      uint8_t *StubTargetAddr = createStubFunction(Section.Address +
                                                   Section.StubOffset);

      // Creating Hi and Lo relocations for the filled stub instructions.
      RelocationEntry REHi(Rel.SectionID,
                           StubTargetAddr - Section.Address,
                           ELF::R_MIPS_HI16, Value.Addend);
      RelocationEntry RELo(Rel.SectionID,
                           StubTargetAddr - Section.Address + 4,
                           ELF::R_MIPS_LO16, Value.Addend);

      if (Value.SymbolName) {
        addRelocationForSymbol(REHi, Value.SymbolName);
        addRelocationForSymbol(RELo, Value.SymbolName);
      } else {
        addRelocationForSection(REHi, Value.SectionID);
        addRelocationForSection(RELo, Value.SectionID);
      }

      resolveRelocation(Target, (uint64_t)Target,
                        (uint64_t)Section.Address +
                        Section.StubOffset, RelType, 0);
      Section.StubOffset += getMaxStubSize();
    }
  } else {
    RelocationEntry RE(Rel.SectionID, Rel.Offset, RelType, Value.Addend);
    if (Value.SymbolName)
      addRelocationForSymbol(RE, Value.SymbolName);
    else
      addRelocationForSection(RE, Value.SectionID);
  }
}

bool RuntimeDyldELF::isCompatibleFormat(const ObjectBuffer *Buffer) const {
  if (Buffer->getBufferSize() < strlen(ELF::ElfMagic))
    return false;
  return (memcmp(Buffer->getBufferStart(), ELF::ElfMagic, strlen(ELF::ElfMagic))) == 0;
}
} // namespace llvm