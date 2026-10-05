-- Transient dual-CPU shared-mailbox probe for System 12.
if _G.SYSTEM12_MAILBOX_PROBE_DONE then return end
_G.SYSTEM12_MAILBOX_PROBE_DONE = true

local m = manager.machine
local sub = m.memory.regions[':sub']
local mainrom = m.memory.regions[':maincpu:rom']
local subcpu = m.devices[':sub']
local maincpu = m.devices[':maincpu']
local subspace = subcpu and subcpu.spaces['program']
local mainspace = maincpu and maincpu.spaces['program']
assert(sub and sub.size == 0x80000, 'missing System 12 H8 ROM region')
assert(mainrom and mainrom.size >= 0x100, 'missing PSX boot ROM region')
assert(subcpu and maincpu and subspace and mainspace,
  'missing System 12 CPUs or program spaces')

local function putword(region, address, value)
  region:write_u8(address, math.floor(value / 256) % 256)
  region:write_u8(address + 1, value % 256)
end

local function stamp(t)
  return string.format('%d:%018d', t.seconds, t.attoseconds)
end

local function state_pc(cpu)
  local state = cpu.state
  local entry = state.PC or state.GENPC or state.CURPC
  return entry and entry.value or -1
end

local function hex(value, width)
  return string.format('%0' .. tostring(width) .. 'x', value)
end

-- MIPS little-endian PSX firmware: write 0x1122, poll H8's 0x3344 ACK,
-- publish 0x3344 at 0x1f080004, then park after the acknowledgement.
local main_firmware = {
  0x3c081f08, -- lui t0,0x1f08
  0x34091122, -- ori t1,zero,0x1122
  0xa5090000, -- sh t1,0(t0)
  0x950a0002, -- loop: lhu t2,2(t0)
  0x00000000, -- nop
  0x1140fffd, -- beq t2,zero,loop
  0x00000000, -- branch delay slot
  0x340b3344, -- ori t3,zero,0x3344
  0x154bfffa, -- bne t2,t3,loop
  0x00000000, -- branch delay slot
  0xa50a0004, -- sh t2,4(t0), ACK
  0x0bf0000b, -- j bfc0002c, park on this instruction
  0x00000000, -- delay slot
}

for index, word in ipairs(main_firmware) do
  mainrom:write_u32((index - 1) * 4, word)
end

-- H8/3002 firmware polls the first shared word, then stores 0x3344 at the
-- next word and sleeps. This uses absolute 32-bit shared-RAM addressing.
local h8_firmware = {
  0x6b20, 0x0008, 0x0000, -- MOV.W @0x080000,R0
  0x7920, 0x1122,          -- CMP.W #0x1122,R0
  0x4600,                  -- BNE poll (relative byte patched below)
  0x7900, 0x3344,          -- MOV.W #0x3344,R0
  0x6ba0, 0x0008, 0x0002, -- MOV.W R0,@0x080002
  0x0180,                  -- SLEEP
}
local poll_address = 0x100
local branch_address = poll_address + 5 * 2
local displacement = poll_address - (branch_address + 2)
assert(displacement >= -128 and displacement <= 127, 'H8 polling branch too far')
h8_firmware[6] = 0x4600 | (displacement & 0xff)
putword(sub, 0, 0)
putword(sub, 2, 0x0100)
for index, word in ipairs(h8_firmware) do
  putword(sub, 0x100 + (index - 1) * 2, word)
end

-- Report loaded ROM bytes, rather than assuming the host's duplicate firmware
-- list still describes the executed probe. This also verifies region endian.
local loaded_main, loaded_sub = {}, {}
for index = 1, #main_firmware do
  local offset = (index - 1) * 4
  local value = mainrom:read_u8(offset) + mainrom:read_u8(offset + 1) * 256
    + mainrom:read_u8(offset + 2) * 65536 + mainrom:read_u8(offset + 3) * 16777216
  loaded_main[#loaded_main + 1] = hex(value, 8)
end
for index = 1, #h8_firmware do
  local offset = 0x100 + (index - 1) * 2
  loaded_sub[#loaded_sub + 1] = hex(sub:read_u8(offset) * 256 + sub:read_u8(offset + 1), 4)
end
print('S12MBX_FIRMWARE cpu=main words=' .. table.concat(loaded_main, ','))
print('S12MBX_FIRMWARE cpu=sub words=' .. table.concat(loaded_sub, ','))

local sequence = 0
local rows = {}
local last_read = {}
local stopped = false
local function cpu_cycles(cpu, t)
  -- Preserve exact scheduler timestamps separately; these cycle annotations
  -- are reset-relative grid ceilings, not CPU-internal counters.
  local hz = cpu == 'main' and 25000000 or 16934400
  local period_as = cpu == 'main' and 40000000000 or 1000000000000000000 / hz
  return t.seconds * hz + math.ceil(t.attoseconds / period_as),
         'scheduler_time_ceil'
end

local function normalized_value(cpu, offset, data, mem_mask)
  if cpu == 'sub' then
    if mem_mask == 0xffff then return data % 0x10000 end
    if (mem_mask & 0xff00) ~= 0 and (mem_mask & 0x00ff) == 0 then
      return math.floor(data / 256) % 256
    elseif (mem_mask & 0x00ff) ~= 0 and (mem_mask & 0xff00) == 0 then
      return data % 256
    end
    return data % 0x10000
  end
  if (mem_mask & 0xffff0000) ~= 0 and (mem_mask & 0x0000ffff) == 0 then
    return math.floor(data / 0x10000) % 0x10000
  elseif (mem_mask & 0x0000ffff) ~= 0 and (mem_mask & 0xffff0000) == 0 then
    return data % 0x10000
  end
  return data % 0x100000000
end

local function actual_address(cpu, offset, mem_mask)
  if cpu == 'sub' then
    if mem_mask == 0xffff then return offset end
    return offset + (((mem_mask & 0xff00) ~= 0) and 0 or 1)
  end
  if (mem_mask & 0xffff0000) ~= 0 and (mem_mask & 0x0000ffff) == 0 then
    return offset + 2
  end
  return offset
end

local function capture(cpu, kind, offset, data, mem_mask)
  sequence = sequence + 1
  local t = m.time
  local cycles, cycle_source = cpu_cycles(cpu, t)
  return {
    sequence = sequence,
    cpu = cpu,
    kind = kind,
    address = actual_address(cpu, offset, mem_mask),
    offset = offset,
    value = normalized_value(cpu, offset, data, mem_mask),
    data = data,
    mem_mask = mem_mask,
    time = { seconds = t.seconds, attoseconds = t.attoseconds },
    pc = state_pc(cpu == 'main' and maincpu or subcpu),
    cycles = cycles,
    cycle_source = cycle_source,
  }
end

local function append_row(row, kind)
  local copy = {}
  for key, value in pairs(row) do copy[key] = value end
  copy.kind = kind or copy.kind
  rows[#rows + 1] = copy
end

local function tap_read(cpu, offset, data, mem_mask)
  local row = capture(cpu, 'read', offset, data, mem_mask)
  local previous = last_read[cpu]
  local same = previous and previous.address == row.address and previous.value == row.value
    and previous.data == row.data and previous.mem_mask == row.mem_mask
  if not same then
    if previous then append_row(previous, 'read_last_before_change') end
    append_row(row, 'read_value_change')
  end
  last_read[cpu] = row
end

local function tap_write(cpu, offset, data, mem_mask)
  local row = capture(cpu, 'write', offset, data, mem_mask)
  append_row(row)
  if cpu == 'main' and row.address == 0x1f080004 then
    for _, cpu_name in ipairs({ 'main', 'sub' }) do
      if last_read[cpu_name] then append_row(last_read[cpu_name], 'read_last_at_ack') end
    end
    table.sort(rows, function(a, b) return a.sequence < b.sequence end)
    for _, event in ipairs(rows) do
      print(string.format(
        'S12MBX_EVENT sequence=%d cpu=%s kind=%s address=%08x offset=%08x value=%08x data=%08x mem_mask=%08x time=%s cycles=%d cycle_source=%s pc=%08x',
        event.sequence, event.cpu, event.kind, event.address, event.offset,
        event.value, event.data, event.mem_mask, stamp(event.time), event.cycles,
        event.cycle_source, event.pc))
    end
    print('S12MBX_ACK_COMPLETE')
    stopped = true
    m:exit()
  end
end

local function in_mailbox(cpu, offset)
  if cpu == 'main' then return offset >= 0x1f080000 and offset <= 0x1f080007 end
  return offset >= 0x080000 and offset <= 0x080005
end

local function tap_dispatch(cpu, kind, offset, data, mem_mask)
  if not in_mailbox(cpu, offset) then return end
  if kind == 'read' then tap_read(cpu, offset, data, mem_mask)
  else tap_write(cpu, offset, data, mem_mask) end
end

-- MAME's PSX and H8 maps install the shared RAM tap handlers in wider map
-- regions; filter their real byte offsets here. The narrower overlapping taps
-- do not run on these direct RAM handlers in current MAME.
local taps = {}
taps[#taps + 1] = mainspace:install_read_tap(0x1f000000, 0x1f0fffff,
  'system12-mailbox-main-read', function(offset, data, mem_mask)
    tap_dispatch('main', 'read', offset, data, mem_mask)
  end)
taps[#taps + 1] = mainspace:install_write_tap(0x1f000000, 0x1f0fffff,
  'system12-mailbox-main-write', function(offset, data, mem_mask)
    tap_dispatch('main', 'write', offset, data, mem_mask)
  end)
taps[#taps + 1] = subspace:install_read_tap(0, 0x00ffffff,
  'system12-mailbox-h8-read', function(offset, data, mem_mask)
    tap_dispatch('sub', 'read', offset, data, mem_mask)
  end)
taps[#taps + 1] = subspace:install_write_tap(0, 0x00ffffff,
  'system12-mailbox-h8-write', function(offset, data, mem_mask)
    tap_dispatch('sub', 'write', offset, data, mem_mask)
  end)
_G.SYSTEM12_MAILBOX_TAPS = taps

local diagnostic_frames = 0
emu.register_frame_done(function()
  diagnostic_frames = diagnostic_frames + 1
  if diagnostic_frames == 4 and not stopped then
    print(string.format('S12MBX_DIAGNOSTIC_EXIT frames=%d main_pc=%08x sub_pc=%08x main_shared=%08x sub_shared=%08x main_ack=%08x sub_ack=%08x captured=%d',
      diagnostic_frames, state_pc(maincpu), state_pc(subcpu),
      mainspace:read_u16(0x1f080000), subspace:read_u16(0x080000),
      mainspace:read_u16(0x1f080004), subspace:read_u16(0x080004), #rows))
    table.sort(rows, function(a, b) return a.sequence < b.sequence end)
    for _, event in ipairs(rows) do
      print(string.format('S12MBX_DIAG_EVENT sequence=%d cpu=%s kind=%s address=%08x offset=%08x value=%08x data=%08x mem_mask=%08x time=%s pc=%08x',
        event.sequence, event.cpu, event.kind, event.address, event.offset,
        event.value, event.data, event.mem_mask, stamp(event.time), event.pc))
    end
    m:exit()
  end
end, 'frame')

local resets = 0
emu.add_machine_reset_notifier(function()
  resets = resets + 1
  local t = m.time
  print(string.format('S12MBX_RESET n=%d time=%s main_pc=%08x sub_pc=%08x',
    resets, stamp(t), state_pc(maincpu), state_pc(subcpu)))
end)

m:soft_reset()
