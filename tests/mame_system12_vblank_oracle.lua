-- Transient H8 port-B/vblank probe; all ROM edits are in memory after load.
if _G.SYSTEM12_VBLANK_PROBE_DONE then return end
_G.SYSTEM12_VBLANK_PROBE_DONE = true

local m = manager.machine
local sub = m.memory.regions[':sub']
local mainrom = m.memory.regions[':maincpu:rom']
local subcpu = m.devices[':sub']
local subspace = subcpu.spaces['program']
local maincpu = m.devices[':maincpu']
local screen = m.screens[':screen']
assert(sub and sub.size == 0x80000, 'missing System 12 H8 ROM region')
assert(mainrom and mainrom.size >= 8, 'missing MIPS boot ROM region')
assert(subcpu and subspace and maincpu and screen, 'missing CPU program space or screen')

local function putword(region, address, value)
  region:write_u8(address, math.floor(value / 256) % 256)
  region:write_u8(address + 1, value % 256)
end

-- Keep the MIPS CPU out of game/GPU work during the H8 read capture.
-- PSX ROM is little endian; this is J bfc00000 / delay-slot NOP.
mainrom:write_u32(0, 0x0bf00000)
mainrom:write_u32(4, 0x00000000)
assert(mainrom:read_u8(0) == 0 and mainrom:read_u8(1) == 0
  and mainrom:read_u8(2) == 0xf0 and mainrom:read_u8(3) == 0x0b,
  'MIPS loop was not stored in little-endian byte order')

-- H8/3002 Port B is mapped at ffff d6.  Poll in a ten-cycle loop:
-- 0x6a08 = MOV.B @abs16,R0L; 0x40fa = BRA back to 0x100.
putword(sub, 0, 0)
putword(sub, 2, 0x0100)
putword(sub, 0x100, 0x6a08)
putword(sub, 0x102, 0xffd6)
putword(sub, 0x104, 0x40fa)

local function stamp(t)
  return string.format('%d:%018d', t.seconds, t.attoseconds)
end
local reads = 0
local changes = {}
local last_read
local tap = subspace:install_read_tap(0xffffd6, 0xffffd7, 'system12-portb-vblank',
  function(offset, data, mem_mask)
    -- Program space is 16-bit big endian.  Port B is the even-address/high lane.
    local value
    if (mem_mask & 0xff00) ~= 0 then value = math.floor(data / 256) % 256
    else value = data % 256 end
    local t = m.time
    local pc = subcpu.state.PC.value
    local sample = { time = t, pc = pc, value = value }
    reads = reads + 1
    if reads == 1 then
      print(string.format('S12_PORTB_FIRSTREAD value=%02x time=%s pc=%s',
        value, stamp(t), tostring(pc)))
    end
    if last_read and ((last_read.value ~ value) & 0x80) ~= 0 and #changes < 4 then
      local row = { before = last_read, after = sample }
      changes[#changes + 1] = row
      print(string.format(
        'S12_PORTB_TRANSITION n=%d before=%02x before_time=%s before_pc=%s after=%02x after_time=%s after_pc=%s',
        #changes, row.before.value, stamp(row.before.time), tostring(row.before.pc),
        row.after.value, stamp(row.after.time), tostring(row.after.pc)))
    end
    last_read = sample
  end)
_G.SYSTEM12_VBLANK_PORTB_TAP = tap -- keep the mempassthrough handler alive

local reset_count = 0
emu.add_machine_reset_notifier(function()
  reset_count = reset_count + 1
  local t = m.time
  print(string.format('S12_H8_RESET n=%d time=%s pc=%s',
    reset_count, stamp(t), tostring(subcpu.state.PC.value)))
end)

local edge_keys = {}
local edges = {}
local previous_start_target
local captured_prior_start = false
local function edge(kind, t)
  local key = kind .. ':' .. stamp(t)
  if edge_keys[key] then return end
  edge_keys[key] = true
  edges[#edges + 1] = { kind = kind, time = t }
  print(string.format('S12_VBLANK_EDGE kind=%s time=%s', kind, stamp(t)))
end

emu.register_frame_done(function()
  -- These screen APIs return scheduled beam boundary times, not frame-done
  -- timestamps.  Log the exact next start and nearest (current/next) end.
  local now = m.time
  local start_target = now + screen:time_until_vblank_start()
  if previous_start_target and not captured_prior_start then
    -- At the first callback, the current frame's start may already have
    -- passed.  Two successive exact screen targets give the frame interval.
    local screen_period = start_target - previous_start_target
    edge('start', previous_start_target - screen_period)
    captured_prior_start = true
  end
  edge('start', start_target)
  previous_start_target = start_target
  edge('end', now + screen:time_until_vblank_end())
  if #changes >= 4 and #edges >= 4 then
    print(string.format('S12_ORACLE_SUMMARY reads=%d transitions=%d screen_edges=%d',
      reads, #changes, #edges))
    m:exit()
  end
end, 'frame')

m:soft_reset()
