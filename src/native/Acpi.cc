//          Copyright Boston University SESA Group 2013 - 2014.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
#include "Acpi.h"

#include <cinttypes>
#include <mutex>

#include "Clock.h"
#include "Cpu.h"
#include "Debug.h"
#include "E820.h"
#include "Io.h"
#include "Numa.h"
#include "VMem.h"

extern "C" {
#include "acpi.h"  // NOLINT
}

namespace {
const constexpr size_t ACPI_MAX_INIT_TABLES = 32;
ACPI_TABLE_DESC initial_table_storage[ACPI_MAX_INIT_TABLES];

bool initialized = false;

void ParseMadt(const ACPI_TABLE_MADT* madt) {
  auto len = madt->Header.Length;
  auto madt_addr = reinterpret_cast<uintptr_t>(madt);
  auto offset = sizeof(ACPI_TABLE_MADT);
  auto first_cpu = true;
  while (offset < len) {
    auto subtable =
        reinterpret_cast<const ACPI_SUBTABLE_HEADER*>(madt_addr + offset);
    switch (subtable->Type) {
    case ACPI_MADT_TYPE_LOCAL_APIC: {
      auto local_apic = reinterpret_cast<const ACPI_MADT_LOCAL_APIC*>(subtable);
      if (local_apic->LapicFlags & ACPI_MADT_ENABLED) {
        ebbrt::kprintf("Local APIC: ACPI ID: %u APIC ID: %u\n",
                       local_apic->ProcessorId, local_apic->Id);
        // account for early bringup of first cpu
        if (first_cpu) {
          auto cpu = ebbrt::Cpu::GetByIndex(0);
          ebbrt::kbugon(cpu == nullptr, "No first cpu!\n");
          cpu->set_acpi_id(local_apic->ProcessorId);
          cpu->set_apic_id(local_apic->Id);
          first_cpu = false;
        } else {
          auto& cpu = ebbrt::Cpu::Create();
          cpu.set_acpi_id(local_apic->ProcessorId);
          cpu.set_apic_id(local_apic->Id);
        }
      }
      offset += sizeof(ACPI_MADT_LOCAL_APIC);
      break;
    }
    case ACPI_MADT_TYPE_IO_APIC: {
      auto io_apic = reinterpret_cast<const ACPI_MADT_IO_APIC*>(subtable);
      ebbrt::kprintf("IO APIC: ID: %u\n", io_apic->Id);
      offset += sizeof(ACPI_MADT_IO_APIC);
      break;
    }
    case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE: {
      auto interrupt_override =
          reinterpret_cast<const ACPI_MADT_INTERRUPT_OVERRIDE*>(subtable);
      ebbrt::kprintf("Interrupt Override: %u -> %u\n",
                     interrupt_override->SourceIrq,
                     interrupt_override->GlobalIrq);
      offset += sizeof(ACPI_MADT_INTERRUPT_OVERRIDE);
      break;
    }
    case ACPI_MADT_TYPE_NMI_SOURCE:
      ebbrt::kprintf("NMI_SOURCE MADT entry found, unsupported... aborting!\n");
      ebbrt::kabort();
      break;
    case ACPI_MADT_TYPE_LOCAL_APIC_NMI: {
      auto local_apic_nmi =
          reinterpret_cast<const ACPI_MADT_LOCAL_APIC_NMI*>(subtable);
      if (local_apic_nmi->ProcessorId == 0xff) {
        ebbrt::kprintf("NMI on all processors");
      } else {
        ebbrt::kprintf("NMI on processor %u", local_apic_nmi->ProcessorId);
      }
      ebbrt::kprintf(": LINT%u\n", local_apic_nmi->Lint);
      offset += sizeof(ACPI_MADT_LOCAL_APIC_NMI);
      break;
    }
    case ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE:
      ebbrt::kprintf(
          "LOCAL_APIC_OVERRIDE MADT entry found, unsupported... aborting!\n");
      ebbrt::kabort();
      break;
    case ACPI_MADT_TYPE_IO_SAPIC:
      ebbrt::kprintf("IO_SAPIC MADT entry found, unsupported... aborting!\n");
      ebbrt::kabort();
      break;
    case ACPI_MADT_TYPE_LOCAL_SAPIC:
      ebbrt::kprintf(
          "LOCAL_SAPIC MADT entry found, unsupported... aborting!\n");
      ebbrt::kabort();
      break;
    case ACPI_MADT_TYPE_INTERRUPT_SOURCE:
      ebbrt::kprintf(
          "INTERRUPT_SOURCE MADT entry found, unsupported... aborting!\n");
      ebbrt::kabort();
      break;
    case ACPI_MADT_TYPE_LOCAL_X2APIC:
      ebbrt::kprintf(
          "LOCAL_X2APIC MADT entry found, unsupported... aborting!\n");
      ebbrt::kabort();
      break;
    case ACPI_MADT_TYPE_LOCAL_X2APIC_NMI:
      ebbrt::kprintf(
          "LOCAL_X2APIC_NMI MADT entry found, unsupported... aborting!\n");
      ebbrt::kabort();
      break;
    case ACPI_MADT_TYPE_GENERIC_INTERRUPT:
      ebbrt::kprintf(
          "GENERIC_INTERRUPT MADT entry found, unsupported... aborting!\n");
      ebbrt::kabort();
      break;
    case ACPI_MADT_TYPE_GENERIC_DISTRIBUTOR:
      ebbrt::kprintf(
          "GENERIC_DISTRIBUTOR MADT entry found, unsupported... aborting!\n");
      ebbrt::kabort();
      break;
    default:
      ebbrt::kprintf("Unrecognized MADT Entry MADT entry found, unsupported... "
                     "aborting!\n");
      ebbrt::kabort();
      break;
    }
  }
}

void ParseSrat(const ACPI_TABLE_SRAT* srat) {
  auto len = srat->Header.Length;
  auto srat_addr = reinterpret_cast<uintptr_t>(srat);
  auto offset = sizeof(ACPI_TABLE_SRAT);
  while (offset < len) {
    auto subtable =
        reinterpret_cast<const ACPI_SUBTABLE_HEADER*>(srat_addr + offset);
    switch (subtable->Type) {
    case ACPI_SRAT_TYPE_CPU_AFFINITY: {
      auto cpu_affinity =
          reinterpret_cast<const ACPI_SRAT_CPU_AFFINITY*>(subtable);
      if (cpu_affinity->Flags & ACPI_SRAT_CPU_USE_AFFINITY) {
        auto prox_domain = cpu_affinity->ProximityDomainLo |
                           cpu_affinity->ProximityDomainHi[0] << 8 |
                           cpu_affinity->ProximityDomainHi[1] << 16 |
                           cpu_affinity->ProximityDomainHi[2] << 24;
        ebbrt::kprintf("SRAT CPU affinity: %u -> %u\n", cpu_affinity->ApicId,
                       prox_domain);
        auto node = ebbrt::numa::SetupNode(prox_domain);

        ebbrt::numa::MapApicToNode(cpu_affinity->ApicId, node);
        auto cpu = ebbrt::Cpu::GetByApicId(cpu_affinity->ApicId);
        ebbrt::kbugon(cpu == nullptr, "Cannot find cpu listed in SRAT");
        cpu->set_nid(node);
      }
      offset += sizeof(ACPI_SRAT_CPU_AFFINITY);
      break;
    }
    case ACPI_SRAT_TYPE_MEMORY_AFFINITY: {
      auto mem_affinity =
          reinterpret_cast<const ACPI_SRAT_MEM_AFFINITY*>(srat_addr + offset);
      if (mem_affinity->Flags & ACPI_SRAT_MEM_ENABLED) {
        auto start = ebbrt::Pfn::Up(mem_affinity->BaseAddress);
        auto end =
            ebbrt::Pfn::Down(mem_affinity->BaseAddress + mem_affinity->Length);
        ebbrt::kprintf(
            "SRAT Memory affinity: %#018" PRIx64 "-%#018" PRIx64 " -> %u\n",
            start.ToAddr(), end.ToAddr() - 1, mem_affinity->ProximityDomain);

        auto node = ebbrt::numa::SetupNode(mem_affinity->ProximityDomain);

        ebbrt::numa::AddMemBlock(node, start, end);
      }
      offset += sizeof(ACPI_SRAT_MEM_AFFINITY);
      break;
    }
    case ACPI_SRAT_TYPE_X2APIC_CPU_AFFINITY:
      ebbrt::kprintf(
          "X2APIC_CPU_AFFINITY SRAT entry found, unsupported... aborting!\n");
      ebbrt::kabort();
      break;
    }
  }
}
}  // namespace

void ebbrt::acpi::PowerOff() {
  ebbrt::kprintf("Power Off.\n");
  AcpiEnterSleepState(ACPI_STATE_S5);
}

void ebbrt::acpi::BootInit() {
  auto status =
      AcpiInitializeTables(initial_table_storage, ACPI_MAX_INIT_TABLES, false);
  if (ACPI_FAILURE(status)) {
    ebbrt::kprintf("AcpiInitializeTables Failed\n");
    ebbrt::kabort();
  }

  ACPI_TABLE_HEADER* madt;
  status = AcpiGetTable(const_cast<char*>(ACPI_SIG_MADT), 1, &madt);
  if (ACPI_FAILURE(status)) {
    ebbrt::kprintf("Failed to locate MADT\n");
    ebbrt::kabort();
  }
  ParseMadt(reinterpret_cast<ACPI_TABLE_MADT*>(madt));

  ACPI_TABLE_HEADER* srat;
  status = AcpiGetTable(const_cast<char*>(ACPI_SIG_SRAT), 1, &srat);
  if (ACPI_FAILURE(status)) {
    // use only one numa node
    auto node = ebbrt::numa::SetupNode(0);
    // attach all cpus to that node
    for (size_t i = 0; i < Cpu::kMaxCpus; ++i) {
      auto c = Cpu::GetByIndex(i);
      if (c) {
        ebbrt::numa::MapApicToNode(c->apic_id(), node);
        c->set_nid(node);
      }
    }
    // attach all memory blocks to that node
    e820::ForEachUsableRegion([node](const e820::Entry& entry) {
      auto start = ebbrt::Pfn::Up(entry.addr());
      auto end = ebbrt::Pfn::Down(entry.addr() + entry.length());
      ebbrt::numa::AddMemBlock(node, start, end);
    });
  } else {
    ParseSrat(reinterpret_cast<ACPI_TABLE_SRAT*>(srat));
  }
}

ACPI_STATUS AcpiOsInitialize() { return AE_OK; }

ACPI_STATUS AcpiOsTerminate() {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer() {
  ACPI_SIZE rsdp;
  if (ACPI_FAILURE(AcpiFindRootPointer(&rsdp))) {
    ebbrt::kprintf("Failed to find root pointer\n");
    ebbrt::kabort();
  }
  return rsdp;
}

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES* Initval,
                                     ACPI_STRING* NewVal) {
  *NewVal = nullptr;
  return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER* ExistingTable,
                                ACPI_TABLE_HEADER** NewTable) {
  *NewTable = nullptr;
  return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER* ExistingTable,
                                        ACPI_PHYSICAL_ADDRESS* NewAddress,
                                        UINT32* NewTableLength) {
  *NewAddress = 0;
  *NewTableLength = 0;
  return AE_OK;
}

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK* OutHandle) {
  *OutHandle = new std::mutex();
  return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK Handle) { EBBRT_UNIMPLEMENTED(); }

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK Handle) {
  // auto mut = static_cast<std::mutex *>(Handle);
  // mut->lock();
  return AE_OK;
}

void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags) {
  // auto mut = static_cast<std::mutex *>(Handle);
  // mut->unlock();
}

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits,
                                  ACPI_SEMAPHORE* OutHandle) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units,
                                UINT16 Timeout) {
  EBBRT_UNIMPLEMENTED();
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

void* AcpiOsAllocate(ACPI_SIZE Size) {
  EBBRT_UNIMPLEMENTED();
  return nullptr;
}

void AcpiOsFree(void* Memory) { EBBRT_UNIMPLEMENTED(); }

void* AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length) {
  if (!initialized) {
    ebbrt::vmem::EarlyMapMemory(Where, Length);
    return reinterpret_cast<void*>(Where);
  } else {
    EBBRT_UNIMPLEMENTED();
    return nullptr;
  }
}

void AcpiOsUnmapMemory(void* LogicalAddress, ACPI_SIZE Size) {
  if (!initialized) {
    ebbrt::vmem::EarlyUnmapMemory(reinterpret_cast<uint64_t>(LogicalAddress),
                                  Size);
  } else {
    EBBRT_UNIMPLEMENTED();
  }
}

ACPI_STATUS AcpiOsGetPhysicalAddress(void* LogicalAddress,
                                     ACPI_PHYSICAL_ADDRESS* PhysicalAddress) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 InterruptNumber,
                                          ACPI_OSD_HANDLER ServiceRoutine,
                                          void* Context) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 InterruptNumber,
                                         ACPI_OSD_HANDLER ServiceRoutine) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_THREAD_ID AcpiOsGetThreadId(void) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE Type,
                          ACPI_OSD_EXEC_CALLBACK Function, void* Context) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

void AcpiOsWaitEventsComplete(void) { EBBRT_UNIMPLEMENTED(); }

void AcpiOsSleep(UINT64 Milliseconds) { EBBRT_UNIMPLEMENTED(); }

void AcpiOsStall(UINT32 Microseconds) {
  auto duration = std::chrono::microseconds(Microseconds);
  auto end = ebbrt::clock::Wall::Now() + duration;
  while (ebbrt::clock::Wall::Now() < end) {
  }
}

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS Address, UINT32* Value,
                           UINT32 Width) {
  switch (Width) {
  case 8:
    *Value = ebbrt::io::In8(Address);
    break;
  case 16:
    *Value = ebbrt::io::In16(Address);
    break;
  case 32:
    *Value = ebbrt::io::In32(Address);
    break;
  default:
    return AE_BAD_PARAMETER;
  }

  return AE_OK;
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS Address, UINT32 Value,
                            UINT32 Width) {
  switch (Width) {
  case 8:
    ebbrt::io::Out8(Address, Value);
    break;
  case 16:
    ebbrt::io::Out16(Address, Value);
    break;
  case 32:
    ebbrt::io::Out32(Address, Value);
    break;
  default:
    return AE_BAD_PARAMETER;
  }
  return AE_OK;
}

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64* Value,
                             UINT32 Width) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 Value,
                              UINT32 Width) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID* PciId, UINT32 Reg,
                                       UINT64* Value, UINT32 Width) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID* PciId, UINT32 Reg,
                                        UINT64 Value, UINT32 Width) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

BOOLEAN AcpiOsReadable(void* Pointer, ACPI_SIZE Length) {
  EBBRT_UNIMPLEMENTED();
  return false;
}

BOOLEAN AcpiOsWritable(void* Pointer, ACPI_SIZE Length) {
  EBBRT_UNIMPLEMENTED();
  return false;
}

UINT64 AcpiOsGetTimer(void) {
  EBBRT_UNIMPLEMENTED();
  return 0;
}

ACPI_STATUS AcpiOsSignal(UINT32 Function, void* Info) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char* Format, ...) {
  va_list ap;
  va_start(ap, Format);
  ebbrt::kvprintf(Format, ap);
}

void AcpiOsVprintf(const char* Format, va_list Args) {
  ebbrt::kvprintf(Format, Args);
}

void AcpiOsRedirectOutput(void* Destination) { EBBRT_UNIMPLEMENTED(); }

ACPI_STATUS AcpiOsGetLine(char* Buffer, UINT32 BufferLength,
                          UINT32* BytesRead) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_STATUS AcpiOsGetTableByName(char* Signature, UINT32 Instance,
                                 ACPI_TABLE_HEADER** Table,
                                 ACPI_PHYSICAL_ADDRESS* Address) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_STATUS AcpiOsGetTableByIndex(UINT32 Index, ACPI_TABLE_HEADER** Table,
                                  UINT32* Instance,
                                  ACPI_PHYSICAL_ADDRESS* Address) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

ACPI_STATUS AcpiOsGetTableByAddress(ACPI_PHYSICAL_ADDRESS Address,
                                    ACPI_TABLE_HEADER** Table) {
  EBBRT_UNIMPLEMENTED();
  return AE_OK;
}

void* AcpiOsOpenDirectory(char* Pathname, char* WildcardSpec,
                          char RequestedFileType) {
  EBBRT_UNIMPLEMENTED();
  return nullptr;
}

char* AcpiOsGetNextFilename(void* DirHandle) {
  EBBRT_UNIMPLEMENTED();
  return nullptr;
}

void AcpiOsCloseDirectory(void* DirHandle) { EBBRT_UNIMPLEMENTED(); }

constexpr uint32_t GL_ACQUIRED = ~0;
constexpr uint32_t GL_BUSY = 0;
constexpr uint32_t GL_BIT_PENDING = 1;
constexpr uint32_t GL_BIT_OWNED = 2;
constexpr uint32_t GL_BIT_MASK = (GL_BIT_PENDING | GL_BIT_OWNED);
int AcpiOsAcquireGlobalLock(uint32_t* lock) {
  EBBRT_UNIMPLEMENTED();
  return 0;
}

int AcpiOsReleaseGlobalLock(uint32_t* lock) {
  EBBRT_UNIMPLEMENTED();
  return 0;
}
