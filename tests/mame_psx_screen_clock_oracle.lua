-- Transient in-memory PSX GPU screen clock oracle for soulclbrac.
-- The Python runner substitutes ACTIONS and SAMPLES with fixed timelines.
if _G.PSX_SCREEN_CLOCK_ORACLE_DONE then return end
_G.PSX_SCREEN_CLOCK_ORACLE_DONE = true

local actions = __ACTIONS__
local samples = __SAMPLES__
local m = manager.machine
local mainrom = m.memory.regions[':maincpu:rom']
local subrom = m.memory.regions[':sub']
local maincpu = m.devices[':maincpu']
local subcpu = m.devices[':sub']
local memory = maincpu.spaces['program']
local screen = m.screens[':screen']
assert(mainrom and mainrom.size >= 8, 'missing MIPS boot ROM region')
assert(subrom and subrom.size >= 0x102, 'missing H8 ROM region')
assert(memory and screen and subcpu, 'missing main CPU program space, H8, or screen')

-- Keep the MIPS CPU in a known idle loop.  PSX ROM is little-endian:
-- J bfc00000 / NOP.  Park the H8 at SLEEP (0x0180) from reset vector 0x100.
mainrom:write_u32(0, 0x0bf00000)
mainrom:write_u32(4, 0x00000000)
subrom:write_u8(0, 0x00); subrom:write_u8(1, 0x00)
subrom:write_u8(2, 0x01); subrom:write_u8(3, 0x00)
subrom:write_u8(0x100, 0x01); subrom:write_u8(0x101, 0x80)

local function stamp(t)
  return string.format('%d:%018d', t.seconds, t.attoseconds)
end
local function snapshot(label)
  local now = m.time
  local start = now + screen:time_until_vblank_start()
  local finish = now + screen:time_until_vblank_end()
  print(string.format('PSX_CLOCK_SAMPLE label=%s now=%s start=%s finish=%s period=%.15f',
    label, stamp(now), stamp(start), stamp(finish), screen.frame_period))
end
local function write_gp1(action)
  local before = m.time
  if action.word then
    memory:write_u32(0x1f801814, action.word)
  elseif action.kind == 'soft_reset' then
    m:soft_reset()
  else
    error('unknown action kind ' .. tostring(action.kind))
  end
  local after = m.time
  print(string.format('PSX_CLOCK_WRITE label=%s kind=%s word=%s before=%s after=%s',
    action.label, action.kind or 'gp1', action.word and string.format('%08x', action.word) or '-',
    stamp(before), stamp(after)))
end

local reset_count = 0
emu.add_machine_reset_notifier(function()
  reset_count = reset_count + 1
  local now = m.time
  local start = now + screen:time_until_vblank_start()
  local finish = now + screen:time_until_vblank_end()
  local mainpc = maincpu.state.pc and maincpu.state.pc.value or 0
  local h8pc = subcpu.state.PC and subcpu.state.PC.value or 0
  print(string.format('PSX_CLOCK_RESET n=%d now=%s mainpc=%08x h8pc=%08x start=%s finish=%s',
    reset_count, stamp(now), mainpc, h8pc, stamp(start), stamp(finish)))
end)

m:soft_reset()

local timeline = {}
for _, row in ipairs(actions) do
  timeline[#timeline + 1] = { at = row.at, kind = 'action', row = row }
end
for _, row in ipairs(samples) do
  timeline[#timeline + 1] = { at = row.at, kind = 'sample', row = row }
end
table.sort(timeline, function(a, b)
  if a.at == b.at then return a.kind == 'sample' and b.kind ~= 'sample' end
  return a.at < b.at
end)

local elapsed = 0
for _, event in ipairs(timeline) do
  emu.wait(event.at - elapsed)
  elapsed = event.at
  if event.kind == 'action' then write_gp1(event.row)
  else snapshot(event.row.label) end
end
print(string.format('PSX_CLOCK_DONE resets=%d', reset_count))
m:exit()
