#include "cpu_code_cache.h"
#include "common/log.h"
#include "cpu_core.h"
#include "cpu_disasm.h"
#include "system.h"
Log_SetChannel(CPU::CodeCache);

#ifdef WITH_RECOMPILER
#include "cpu_recompiler_code_generator.h"
#include "cpu_recompiler_thunks.h"
#endif

namespace CPU {

constexpr bool USE_BLOCK_LINKING = true;

static constexpr u32 RECOMPILER_CODE_CACHE_SIZE = 32 * 1024 * 1024;
static constexpr u32 RECOMPILER_FAR_CODE_CACHE_SIZE = 32 * 1024 * 1024;

CodeCache::CodeCache() = default;

CodeCache::~CodeCache()
{
  if (m_system)
    Flush();

  ShutdownFastmem();
}

void CodeCache::Initialize(System* system, Core* core, Bus* bus)
{
  m_system = system;
  m_core = core;
  m_bus = bus;
}

void CodeCache::Execute()
{
  if (m_use_recompiler)
  {
    ExecuteRecompiler();
    return;
  }

  CodeBlockKey next_block_key = GetNextBlockKey();

  while (m_core->m_pending_ticks < m_core->m_downcount)
  {
    if (m_core->HasPendingInterrupt())
    {
      // TODO: Fill in m_next_instruction...
      m_core->SafeReadMemoryWord(m_core->m_regs.pc, &m_core->m_next_instruction.bits);
      m_core->DispatchInterrupt();
      next_block_key = GetNextBlockKey();
    }

    CodeBlock* block = LookupBlock(next_block_key);
    if (!block)
    {
      Log_WarningPrintf("Falling back to uncached interpreter at 0x%08X", m_core->GetRegs().pc);
      InterpretUncachedBlock();
      continue;
    }

  reexecute_block:

#if 0
    const u32 tick = m_system->GetGlobalTickCounter() + m_core->GetPendingTicks();
    if (tick == 17426)
      __debugbreak();
#endif

#if 0
    LogCurrentState();
#endif

    if (m_use_recompiler)
      block->host_code(m_core);
    else
      InterpretCachedBlock(*block);

    if (m_core->m_pending_ticks >= m_core->m_downcount)
      break;
    else if (m_core->HasPendingInterrupt() || !USE_BLOCK_LINKING)
      continue;

    next_block_key = GetNextBlockKey();
    if (next_block_key.bits == block->key.bits)
    {
      // we can jump straight to it if there's no pending interrupts
      // ensure it's not a self-modifying block
      if (!block->invalidated || RevalidateBlock(block))
        goto reexecute_block;
    }
    else if (!block->invalidated)
    {
      // Try to find an already-linked block.
      // TODO: Don't need to dereference the block, just store a pointer to the code.
      for (CodeBlock* linked_block : block->link_successors)
      {
        if (linked_block->key.bits == next_block_key.bits)
        {
          if (linked_block->invalidated && !RevalidateBlock(linked_block))
          {
            // CanExecuteBlock can result in a block flush, so stop iterating here.
            break;
          }

          // Execute the linked block
          block = linked_block;
          goto reexecute_block;
        }
      }

      // No acceptable blocks found in the successor list, try a new one.
      CodeBlock* next_block = LookupBlock(next_block_key);
      if (next_block)
      {
        // Link the previous block to this new block if we find a new block.
        LinkBlock(block, next_block);
        block = next_block;
        goto reexecute_block;
      }
    }
  }

  // in case we switch to interpreter...
  m_core->m_regs.npc = m_core->m_regs.pc;
}

void CodeCache::ExecuteRecompiler()
{
  while (m_core->m_pending_ticks < m_core->m_downcount)
  {
    if (m_core->HasPendingInterrupt())
    {
      m_core->SafeReadMemoryWord(m_core->m_regs.pc, &m_core->m_next_instruction.bits);
      m_core->DispatchInterrupt();
    }

    m_block_function_lookup.Dispatch(m_core);
  }

  // in case we switch to interpreter...
  m_core->m_regs.npc = m_core->m_regs.pc;
}

void CodeCache::SetUseRecompiler(bool enable, bool fastmem)
{
#ifdef WITH_RECOMPILER
  if (m_use_recompiler == enable && m_fastmem == fastmem)
    return;

  Flush();

  ShutdownFastmem();
  m_asm_functions.reset();
  m_code_buffer.reset();

  m_use_recompiler = enable;
  m_fastmem = fastmem;

  if (enable)
  {
    m_code_buffer = std::make_unique<JitCodeBuffer>(RECOMPILER_CODE_CACHE_SIZE, RECOMPILER_FAR_CODE_CACHE_SIZE);
    m_asm_functions = std::make_unique<Recompiler::ASMFunctions>();
    m_asm_functions->Generate(m_code_buffer.get());
  }

  InitializeFastmem();
#endif
}

void CodeCache::Flush()
{
  m_bus->ClearRAMCodePageFlags();
  for (auto& it : m_ram_block_map)
    it.clear();

  for (const auto& it : m_blocks)
    delete it.second;
  m_blocks.clear();
  m_host_code_map.clear();
#ifdef WITH_RECOMPILER
  if (m_code_buffer)
    m_code_buffer->Reset();
  m_block_function_lookup.Reset(FastCompileBlockFunction);
#endif
}

void CodeCache::LogCurrentState()
{
  const auto& regs = m_core->m_regs;
  WriteToExecutionLog("tick=%u pc=%08X zero=%08X at=%08X v0=%08X v1=%08X a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X "
                      "t1=%08X t2=%08X t3=%08X t4=%08X t5=%08X t6=%08X t7=%08X s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X "
                      "s5=%08X s6=%08X s7=%08X t8=%08X t9=%08X k0=%08X k1=%08X gp=%08X sp=%08X fp=%08X ra=%08X ldr=%s "
                      "ldv=%08X\n",
                      m_system->GetGlobalTickCounter() + m_core->GetPendingTicks(), regs.pc, regs.zero, regs.at,
                      regs.v0, regs.v1, regs.a0, regs.a1, regs.a2, regs.a3, regs.t0, regs.t1, regs.t2, regs.t3, regs.t4,
                      regs.t5, regs.t6, regs.t7, regs.s0, regs.s1, regs.s2, regs.s3, regs.s4, regs.s5, regs.s6, regs.s7,
                      regs.t8, regs.t9, regs.k0, regs.k1, regs.gp, regs.sp, regs.fp, regs.ra,
                      (m_core->m_next_load_delay_reg == Reg::count) ? "NONE" :
                                                                      GetRegName(m_core->m_next_load_delay_reg),
                      (m_core->m_next_load_delay_reg == Reg::count) ? 0 : m_core->m_next_load_delay_value);
}

CodeBlockKey CodeCache::GetNextBlockKey() const
{
  CodeBlockKey key = {};
  key.SetPC(m_core->GetRegs().pc);
  key.user_mode = m_core->InUserMode();
  return key;
}

CodeBlock* CodeCache::LookupBlock(CodeBlockKey key)
{
  BlockMap::iterator iter = m_blocks.find(key.bits);
  if (iter != m_blocks.end())
  {
    // ensure it hasn't been invalidated
    CodeBlock* existing_block = iter->second;
    if (!existing_block || !existing_block->invalidated || RevalidateBlock(existing_block))
      return existing_block;
  }

  return CompileBlock(key);
}

bool CodeCache::RevalidateBlock(CodeBlock* block)
{
  for (const CodeBlockInstruction& cbi : block->instructions)
  {
    u32 new_code = 0;
    m_bus->DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(cbi.pc & PHYSICAL_MEMORY_ADDRESS_MASK,
                                                                          new_code);
    if (cbi.instruction.bits != new_code)
    {
      Log_DebugPrintf("Block 0x%08X changed at PC 0x%08X - %08X to %08X - recompiling.", block->GetPC(), cbi.pc,
                      cbi.instruction.bits, new_code);
      goto recompile;
    }
  }

  // re-add it to the page map since it's still up-to-date
  block->invalidated = false;
  AddBlockToPageMap(block);
#ifdef WITH_RECOMPILER
  m_block_function_lookup.SetBlockPointer(block->GetPC(), block->host_code);
#endif
  return true;

recompile:
  RemoveBlockFromHostCodeMap(block);

  block->instructions.clear();
  if (!CompileBlock(block))
  {
    Log_WarningPrintf("Failed to recompile block 0x%08X - flushing.", block->GetPC());
    FlushBlock(block);
    return false;
  }

  // re-add to page map again
  AddBlockToHostCodeMap(block);
  if (block->IsInRAM())
    AddBlockToPageMap(block);

  return true;
}

void CodeCache::FastCompileBlockFunction(CPU::Core* cpu)
{
  CodeCache* cc = cpu->m_system->GetCPUCodeCache();
  CodeBlock* block = cc->LookupBlock(cc->GetNextBlockKey());
  if (block)
    block->host_code(cpu);
  else
    cc->InterpretUncachedBlock();
}

CodeBlock* CodeCache::CompileBlock(CodeBlockKey key)
{
  CodeBlock* block = new CodeBlock(key);
  if (CompileBlock(block))
  {
    // add it to the page map if it's in ram
    AddBlockToPageMap(block);
  }
  else
  {
    Log_ErrorPrintf("Failed to compile block at PC=0x%08X", key.GetPC());
    delete block;
    block = nullptr;
  }

  m_blocks.emplace(key.bits, block);
  AddBlockToHostCodeMap(block);
#ifdef WITH_RECOMPILER
  m_block_function_lookup.SetBlockPointer(block->GetPC(), block->host_code);
#endif
  return block;
}

bool CodeCache::CompileBlock(CodeBlock* block)
{
  u32 pc = block->GetPC();
  bool is_branch_delay_slot = false;
  bool is_load_delay_slot = false;

#if 0
  if (pc == 0x0005aa90)
    __debugbreak();
#endif

  for (;;)
  {
    CodeBlockInstruction cbi = {};

    const PhysicalMemoryAddress phys_addr = pc & PHYSICAL_MEMORY_ADDRESS_MASK;
    if (!m_bus->IsCacheableAddress(phys_addr) ||
        m_bus->DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(phys_addr, cbi.instruction.bits) < 0 ||
        !IsInvalidInstruction(cbi.instruction))
    {
      break;
    }

    cbi.pc = pc;
    cbi.is_branch_delay_slot = is_branch_delay_slot;
    cbi.is_load_delay_slot = is_load_delay_slot;
    cbi.is_branch_instruction = IsBranchInstruction(cbi.instruction);
    cbi.is_load_instruction = IsMemoryLoadInstruction(cbi.instruction);
    cbi.is_store_instruction = IsMemoryStoreInstruction(cbi.instruction);
    cbi.has_load_delay = InstructionHasLoadDelay(cbi.instruction);
    cbi.can_trap = CanInstructionTrap(cbi.instruction, m_core->InUserMode());

    block->contains_loadstore_instructions |= cbi.is_load_instruction;
    block->contains_loadstore_instructions |= cbi.is_store_instruction;

    // instruction is decoded now
    block->instructions.push_back(cbi);
    pc += sizeof(cbi.instruction.bits);

    // if we're in a branch delay slot, the block is now done
    // except if this is a branch in a branch delay slot, then we grab the one after that, and so on...
    if (is_branch_delay_slot && !cbi.is_branch_instruction)
      break;

    // if this is a branch, we grab the next instruction (delay slot), and then exit
    is_branch_delay_slot = cbi.is_branch_instruction;

    // same for load delay
    is_load_delay_slot = cbi.has_load_delay;

    // is this a non-branchy exit? (e.g. syscall)
    if (IsExitBlockInstruction(cbi.instruction))
      break;
  }

  if (!block->instructions.empty())
  {
    block->instructions.back().is_last_instruction = true;

#ifdef _DEBUG
    SmallString disasm;
    Log_DebugPrintf("Block at 0x%08X", block->GetPC());
    for (const CodeBlockInstruction& cbi : block->instructions)
    {
      CPU::DisassembleInstruction(&disasm, cbi.pc, cbi.instruction.bits, nullptr);
      Log_DebugPrintf("[%s %s 0x%08X] %08X %s", cbi.is_branch_delay_slot ? "BD" : "  ",
                      cbi.is_load_delay_slot ? "LD" : "  ", cbi.pc, cbi.instruction.bits, disasm.GetCharArray());
    }
#endif
  }
  else
  {
    Log_WarningPrintf("Empty block compiled at 0x%08X", block->key.GetPC());
    return false;
  }

#ifdef WITH_RECOMPILER
  if (m_use_recompiler)
  {
    // Ensure we're not going to run out of space while compiling this block.
    if (m_code_buffer->GetFreeCodeSpace() <
          (block->instructions.size() * Recompiler::MAX_NEAR_HOST_BYTES_PER_INSTRUCTION) ||
        m_code_buffer->GetFreeFarCodeSpace() <
          (block->instructions.size() * Recompiler::MAX_FAR_HOST_BYTES_PER_INSTRUCTION))
    {
      Log_WarningPrintf("Out of code space, flushing all blocks.");
      Flush();
    }

    Recompiler::CodeGenerator codegen(m_core, m_code_buffer.get(), *m_asm_functions.get(), m_fastmem);
    if (!codegen.CompileBlock(block, &block->host_code, &block->host_code_size))
    {
      Log_ErrorPrintf("Failed to compile host code for block at 0x%08X", block->key.GetPC());
      return false;
    }
  }
#endif

  return true;
}

void CodeCache::InvalidateBlocksWithPageIndex(u32 page_index)
{
  DebugAssert(page_index < CPU_CODE_CACHE_PAGE_COUNT);
  auto& blocks = m_ram_block_map[page_index];
  for (CodeBlock* block : blocks)
  {
    // Invalidate forces the block to be checked again.
    Log_DebugPrintf("Invalidating block at 0x%08X", block->GetPC());
    block->invalidated = true;
#ifdef WITH_RECOMPILER
    m_block_function_lookup.SetBlockPointer(block->GetPC(), FastCompileBlockFunction);
#endif
  }

  // Block will be re-added next execution.
  blocks.clear();
  m_bus->ClearRAMCodePage(page_index);
}

void CodeCache::FlushBlock(CodeBlock* block)
{
  BlockMap::iterator iter = m_blocks.find(block->key.GetPC());
  Assert(iter != m_blocks.end() && iter->second == block);
  Log_DevPrintf("Flushing block at address 0x%08X", block->GetPC());

  // if it's been invalidated it won't be in the page map
  if (block->invalidated)
    RemoveBlockFromPageMap(block);

  RemoveBlockFromHostCodeMap(block);

#ifdef WITH_RECOMPILER
  m_block_function_lookup.SetBlockPointer(block->GetPC(), FastCompileBlockFunction);
#endif

  m_blocks.erase(iter);
  delete block;
}

void CodeCache::AddBlockToPageMap(CodeBlock* block)
{
  if (!block->IsInRAM())
    return;

  const u32 start_page = block->GetStartPageIndex();
  const u32 end_page = block->GetEndPageIndex();
  for (u32 page = start_page; page <= end_page; page++)
  {
    m_ram_block_map[page].push_back(block);
    m_bus->SetRAMCodePage(page);
  }
}

void CodeCache::RemoveBlockFromPageMap(CodeBlock* block)
{
  if (!block->IsInRAM())
    return;

  const u32 start_page = block->GetStartPageIndex();
  const u32 end_page = block->GetEndPageIndex();
  for (u32 page = start_page; page <= end_page; page++)
  {
    auto& page_blocks = m_ram_block_map[page];
    auto page_block_iter = std::find(page_blocks.begin(), page_blocks.end(), block);
    Assert(page_block_iter != page_blocks.end());
    page_blocks.erase(page_block_iter);
  }
}

void CodeCache::AddBlockToHostCodeMap(CodeBlock* block)
{
  if (!m_use_recompiler)
    return;

  auto ir = m_host_code_map.emplace(block->host_code, block);
  Assert(ir.second);
}

void CodeCache::RemoveBlockFromHostCodeMap(CodeBlock* block)
{
  if (!m_use_recompiler)
    return;

  HostCodeMap::iterator hc_iter = m_host_code_map.find(block->host_code);
  Assert(hc_iter != m_host_code_map.end());
  m_host_code_map.erase(hc_iter);
}

void CodeCache::LinkBlock(CodeBlock* from, CodeBlock* to)
{
  Log_DebugPrintf("Linking block %p(%08x) to %p(%08x)", from, from->GetPC(), to, to->GetPC());
  from->link_successors.push_back(to);
  to->link_predecessors.push_back(from);
}

void CodeCache::UnlinkBlock(CodeBlock* block)
{
  for (CodeBlock* predecessor : block->link_predecessors)
  {
    auto iter = std::find(predecessor->link_successors.begin(), predecessor->link_successors.end(), block);
    Assert(iter != predecessor->link_successors.end());
    predecessor->link_successors.erase(iter);
  }
  block->link_predecessors.clear();

  for (CodeBlock* successor : block->link_successors)
  {
    auto iter = std::find(successor->link_predecessors.begin(), successor->link_predecessors.end(), block);
    Assert(iter != successor->link_predecessors.end());
    successor->link_predecessors.erase(iter);
  }
  block->link_successors.clear();
}

void CodeCache::InterpretCachedBlock(const CodeBlock& block)
{
  // set up the state so we've already fetched the instruction
  DebugAssert(m_core->m_regs.pc == block.GetPC());

  m_core->m_regs.npc = block.GetPC() + 4;

  for (const CodeBlockInstruction& cbi : block.instructions)
  {
    m_core->m_pending_ticks++;

    // now executing the instruction we previously fetched
    m_core->m_current_instruction.bits = cbi.instruction.bits;
    m_core->m_current_instruction_pc = cbi.pc;
    m_core->m_current_instruction_in_branch_delay_slot = cbi.is_branch_delay_slot;
    m_core->m_current_instruction_was_branch_taken = m_core->m_branch_was_taken;
    m_core->m_branch_was_taken = false;
    m_core->m_exception_raised = false;

    // update pc
    m_core->m_regs.pc = m_core->m_regs.npc;
    m_core->m_regs.npc += 4;

    // execute the instruction we previously fetched
    m_core->ExecuteInstruction();

    // next load delay
    m_core->UpdateLoadDelay();

    if (m_core->m_exception_raised)
      break;
  }

  // cleanup so the interpreter can kick in if needed
  m_core->m_next_instruction_is_branch_delay_slot = false;
}

void CodeCache::InterpretUncachedBlock()
{
  Panic("Fixme with regards to re-fetching PC");

  // At this point, pc contains the last address executed (in the previous block). The instruction has not been fetched
  // yet. pc shouldn't be updated until the fetch occurs, that way the exception occurs in the delay slot.
  bool in_branch_delay_slot = false;
  for (;;)
  {
    m_core->m_pending_ticks++;

    // now executing the instruction we previously fetched
    m_core->m_current_instruction.bits = m_core->m_next_instruction.bits;
    m_core->m_current_instruction_pc = m_core->m_regs.pc;
    m_core->m_current_instruction_in_branch_delay_slot = m_core->m_next_instruction_is_branch_delay_slot;
    m_core->m_current_instruction_was_branch_taken = m_core->m_branch_was_taken;
    m_core->m_next_instruction_is_branch_delay_slot = false;
    m_core->m_branch_was_taken = false;
    m_core->m_exception_raised = false;

    // Fetch the next instruction, except if we're in a branch delay slot. The "fetch" is done in the next block.
    if (!m_core->FetchInstruction())
      break;

    // execute the instruction we previously fetched
    m_core->ExecuteInstruction();

    // next load delay
    m_core->UpdateLoadDelay();

    const bool branch = IsBranchInstruction(m_core->m_current_instruction);
    if (m_core->m_exception_raised || (!branch && in_branch_delay_slot) ||
        IsExitBlockInstruction(m_core->m_current_instruction))
    {
      break;
    }

    in_branch_delay_slot = branch;
  }
}

bool CodeCache::InitializeFastmem()
{
  if (!m_use_recompiler || !m_fastmem)
    return true;

  if (!Common::PageFaultHandler::InstallHandler(this,
                                                std::bind(&CodeCache::PageFaultHandler, this, std::placeholders::_1,
                                                          std::placeholders::_2, std::placeholders::_3)))
  {
    Log_ErrorPrintf("Failed to install page fault handler");
    return false;
  }

  m_bus->UpdateFastmemViews(true, m_core->m_cop0_regs.sr.Isc);
  return true;
}

void CodeCache::ShutdownFastmem()
{
  if (!m_use_recompiler || !m_fastmem)
    return;

  Common::PageFaultHandler::RemoveHandler(this);
  m_bus->UpdateFastmemViews(false, false);
}

Common::PageFaultHandler::HandlerResult CodeCache::PageFaultHandler(void* exception_pc, void* fault_address,
                                                                    bool is_write)
{
  if (static_cast<u8*>(fault_address) < m_core->m_fastmem_base ||
      (static_cast<u8*>(fault_address) - m_core->m_fastmem_base) >= Bus::FASTMEM_REGION_SIZE)
  {
    return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;
  }

  const PhysicalMemoryAddress fastmem_address = static_cast<PhysicalMemoryAddress>(
    static_cast<ptrdiff_t>(static_cast<u8*>(fault_address) - m_core->m_fastmem_base));

  Log_DevPrintf("Page fault handler invoked at PC=%p Address=%p %s, fastmem offset 0x%08X", exception_pc, fault_address,
                is_write ? "(write)" : "(read)", fastmem_address);

  if (is_write && !m_core->m_cop0_regs.sr.Isc && m_bus->IsRAMAddress(fastmem_address))
  {
    // this is probably a code page, since we aren't going to fault due to requiring fastmem on RAM.
    const u32 code_page_index = m_bus->GetRAMCodePageIndex(fastmem_address);
    if (m_bus->IsRAMCodePage(code_page_index))
    {
      InvalidateBlocksWithPageIndex(code_page_index);
      return Common::PageFaultHandler::HandlerResult::ContinueExecution;
    }
  }

  // use upper_bound to find the next block after the pc
  HostCodeMap::iterator upper_iter =
    m_host_code_map.upper_bound(reinterpret_cast<CodeBlock::HostCodePointer>(exception_pc));
  if (upper_iter == m_host_code_map.begin())
    return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;

  // then decrement it by one to (hopefully) get the block we want
  upper_iter--;

  // find the loadstore info in the code block
  CodeBlock* block = upper_iter->second;
  for (auto bpi_iter = block->loadstore_backpatch_info.begin(); bpi_iter != block->loadstore_backpatch_info.end();
       ++bpi_iter)
  {
    const Recompiler::LoadStoreBackpatchInfo& lbi = *bpi_iter;
    if (lbi.host_pc == exception_pc)
    {
      // found it, do fixup
      if (Recompiler::CodeGenerator::BackpatchLoadStore(lbi))
      {
        // remove the backpatch entry since we won't be coming back to this one
        block->loadstore_backpatch_info.erase(bpi_iter);
        return Common::PageFaultHandler::HandlerResult::ContinueExecution;
      }
      else
      {
        Log_ErrorPrintf("Failed to backpatch %p in block 0x%08X", exception_pc, block->GetPC());
        return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;
      }
    }
  }

  // we didn't find the pc in our list..
  Log_ErrorPrintf("Loadstore PC not found for %p in block 0x%08X", exception_pc, block->GetPC());
  return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;
}

} // namespace CPU
