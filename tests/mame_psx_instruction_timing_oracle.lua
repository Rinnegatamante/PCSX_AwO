-- Real MAME PSX instruction timing markers for System 12.
if _G.SYSTEM12_PSX_TIMING_DONE then return end
_G.SYSTEM12_PSX_TIMING_DONE = true

local m = manager.machine
local mainrom = m.memory.regions[':maincpu:rom']
local sub = m.memory.regions[':sub']
local maincpu = m.devices[':maincpu']
local subcpu = m.devices[':sub']
local mainspace = maincpu and maincpu.spaces['program']
assert(mainrom and mainrom.size >= 0x100, 'missing PSX boot ROM region')
assert(sub and sub.size == 0x80000, 'missing H8 ROM region')
assert(mainspace and subcpu, 'missing System 12 CPUs or main program space')

local function stamp(t)
  return string.format('%d:%018d', t.seconds, t.attoseconds)
end

local function hex(value, width)
  return string.format('%0' .. tostring(width) .. 'x', value)
end

local main_firmware = { MAIN_FIRMWARE }
for index, word in ipairs(main_firmware) do
  mainrom:write_u32((index - 1) * 4, word)
end

-- H8 reset vector points to an inert SLEEP instruction.
sub:write_u8(0, 0)
sub:write_u8(1, 0)
sub:write_u8(2, 1)
sub:write_u8(3, 0)
sub:write_u8(0x100, 0x01)
sub:write_u8(0x101, 0x80)

local loaded = {}
for index = 1, #main_firmware do
  local offset = (index - 1) * 4
  local word = mainrom:read_u8(offset) + mainrom:read_u8(offset + 1) * 256
    + mainrom:read_u8(offset + 2) * 65536 + mainrom:read_u8(offset + 3) * 16777216
  loaded[#loaded + 1] = hex(word, 8)
end
print('S12PSXTIMING_FIRMWARE words=' .. table.concat(loaded, ','))

local function pc_value()
  local state = maincpu.state
  local entry = state and (state.PC or state.GENPC or state.CURPC)
  return entry and entry.value or -1
end

local marker_by_address = {}
for index = 0, 7 do marker_by_address[0x1f080000 + index * 2] = 0xa000 + index end
local sequence = 0
local stores = {}
local raw_count = 0
local function write_tap(offset, data, mem_mask)
  if offset < 0x1f080000 or offset > 0x1f08000f then return end
  if raw_count < 16 then
    raw_count = raw_count + 1
    print(string.format('S12PSXTIMING_RAW offset=%08x data=%08x mem_mask=%08x time=%s pc=%08x',
      offset, data, mem_mask, stamp(m.time), pc_value()))
  end
  local address, value
  if (mem_mask & 0xffff0000) ~= 0 and (mem_mask & 0x0000ffff) == 0 then
    address, value = offset + 2, math.floor(data / 65536) % 65536
  elseif (mem_mask & 0x0000ffff) ~= 0 and (mem_mask & 0xffff0000) == 0 then
    address, value = offset, data % 65536
  else
    return
  end
  if marker_by_address[address] ~= value then return end
  sequence = sequence + 1
  local t = m.time
  local width = (mem_mask == 0xffff or mem_mask == 0xffff0000) and 16 or 32
  local row = { sequence = sequence, address = address, value = value,
    width = width, mem_mask = mem_mask,
    time = { seconds = t.seconds, attoseconds = t.attoseconds }, pc = pc_value() }
  stores[#stores + 1] = row
  print(string.format('S12PSXTIMING_STORE sequence=%d address=%08x value=%04x width=%d mem_mask=%08x time=%s pc=%08x',
    row.sequence, row.address, row.value, width, mem_mask, stamp(t), row.pc))
  if address == 0x1f08000e then
    print('S12PSXTIMING_COMPLETE')
    m:exit()
  end
end

-- Taps on the PSX RAM handler must cover the mapped direct-RAM range.
local tap = mainspace:install_write_tap(0x1f000000, 0x1f0fffff,
  'system12-psx-instruction-timing', write_tap)
_G.SYSTEM12_PSX_TIMING_TAP = tap

local resets = 0
emu.add_machine_reset_notifier(function()
  resets = resets + 1
  print(string.format('S12PSXTIMING_RESET n=%d time=%s pc=%08x',
    resets, stamp(m.time), pc_value()))
end)

local frames = 0
emu.register_frame_done(function()
  frames = frames + 1
  if frames == 2 and #stores < 8 then
    print(string.format('S12PSXTIMING_DIAGNOSTIC frames=%d pc=%08x stores=%d',
      frames, pc_value(), #stores))
    m:exit()
  end
end, 'frame')

m:soft_reset()
