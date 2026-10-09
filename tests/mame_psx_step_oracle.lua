-- Bounded real-MAME PSX control-flow/step probe.
if _G.SYSTEM12_PSX_STEP_DONE then return end
_G.SYSTEM12_PSX_STEP_DONE = true

local CASE_NAME = '@CASE_NAME@'
local STOP_ADDRESS = 0x@STOP_ADDRESS@
local STOP_VALUE = 0x@STOP_VALUE@
local m = manager.machine
local mainrom = m.memory.regions[':maincpu:rom']
local sub = m.memory.regions[':sub']
local maincpu = m.devices[':maincpu']
local subcpu = m.devices[':sub']
local mainspace = maincpu and maincpu.spaces['program']
assert(mainrom and mainrom.size >= 0x200, 'missing PSX boot/exception ROM')
assert(sub and sub.size == 0x80000 and mainspace and subcpu,
  'missing System 12 CPU ROMs or program space')

local function put32(region, offset, value)
  region:write_u32(offset, value)
end
local function put16be(region, offset, value)
  region:write_u8(offset, math.floor(value / 256) % 256)
  region:write_u8(offset + 1, value % 256)
end
local function stamp(t)
  return string.format('%d:%018d', t.seconds, t.attoseconds)
end
local function hex(value, width)
  return string.format('%0' .. tostring(width) .. 'x', value)
end
local function pc_value()
  local state = maincpu.state
  local entry = state and (state.PC or state.GENPC or state.CURPC)
  return entry and entry.value or -1
end

local main_firmware = { @MAIN_FIRMWARE@ }
local vector_firmware = { @VECTOR_FIRMWARE@ }
for i, word in ipairs(main_firmware) do put32(mainrom, (i - 1) * 4, word) end
for i, word in ipairs(vector_firmware) do put32(mainrom, 0x180 + (i - 1) * 4, word) end
-- H8 remains asleep and cannot create shared-memory traffic.
put16be(sub, 0, 0)
put16be(sub, 2, 0x0100)
put16be(sub, 0x100, 0x0180)

local main_readback, vector_readback = {}, {}
for i = 1, #main_firmware do
  local offset = (i - 1) * 4
  main_readback[#main_readback + 1] = hex(mainrom:read_u8(offset)
    + mainrom:read_u8(offset + 1) * 256 + mainrom:read_u8(offset + 2) * 65536
    + mainrom:read_u8(offset + 3) * 16777216, 8)
end
for i = 1, #vector_firmware do
  local offset = 0x180 + (i - 1) * 4
  vector_readback[#vector_readback + 1] = hex(mainrom:read_u8(offset)
    + mainrom:read_u8(offset + 1) * 256 + mainrom:read_u8(offset + 2) * 65536
    + mainrom:read_u8(offset + 3) * 16777216, 8)
end
print(string.format('S12STEP_FIRMWARE case=%s main=%s vector=%s h8=0180', CASE_NAME,
  table.concat(main_readback, ','), table.concat(vector_readback, ',')))

local sequence, stores = 0, {}
local function write_tap(offset, data, mem_mask)
  if offset < 0x1f080000 or offset > 0x1f08003f then return end
  local address, value, width
  if mem_mask == 0xffffffff then
    address, value, width = offset, data % 0x100000000, 32
  elseif (mem_mask & 0xffff0000) ~= 0 and (mem_mask & 0x0000ffff) == 0 then
    address, value, width = offset + 2, math.floor(data / 65536) % 65536, 16
  elseif (mem_mask & 0x0000ffff) ~= 0 and (mem_mask & 0xffff0000) == 0 then
    address, value, width = offset, data % 65536, 16
  else
    return
  end
  sequence = sequence + 1
  local t = m.time
  local row = { sequence = sequence, address = address, value = value,
    width = width, mem_mask = mem_mask,
    time = { seconds = t.seconds, attoseconds = t.attoseconds }, pc = pc_value() }
  stores[#stores + 1] = row
  print(string.format('S12STEP_STORE case=%s sequence=%d address=%08x value=%08x width=%d mem_mask=%08x time=%s pc=%08x',
    CASE_NAME, row.sequence, row.address, row.value, row.width, row.mem_mask,
    stamp(t), row.pc))
  if address == STOP_ADDRESS and value == STOP_VALUE then
    print('S12STEP_COMPLETE case=' .. CASE_NAME)
    m:exit()
  end
end

local tap = mainspace:install_write_tap(0x1f000000, 0x1f0fffff,
  'system12-psx-step-store', write_tap)
_G.SYSTEM12_PSX_STEP_TAP = tap

local resets = 0
emu.add_machine_reset_notifier(function()
  resets = resets + 1
  print(string.format('S12STEP_RESET case=%s n=%d time=%s pc=%08x',
    CASE_NAME, resets, stamp(m.time), pc_value()))
end)

local frames = 0
emu.register_frame_done(function()
  frames = frames + 1
  if frames == 2 and #stores == 0 then
    print(string.format('S12STEP_DIAGNOSTIC case=%s frames=%d pc=%08x stores=%d',
      CASE_NAME, frames, pc_value(), #stores))
    m:exit()
  elseif frames == 2 and #stores > 0 then
    print(string.format('S12STEP_DIAGNOSTIC case=%s frames=%d pc=%08x stores=%d',
      CASE_NAME, frames, pc_value(), #stores))
    m:exit()
  end
end, 'frame')

m:soft_reset()
