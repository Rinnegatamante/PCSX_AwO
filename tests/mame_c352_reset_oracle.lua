-- Actual-MAME C352 stream epoch probe across a soft reset.
-- The Python runner substitutes the NOP count and captures structured output.
if _G.C352_RESET_PROBE_DONE then return end
_G.C352_RESET_PROBE_DONE = true
local keyon_delay_nops = __KEYON_DELAY_NOPS__
local m = manager.machine
local sub = m.memory.regions[':sub']
local wave = m.memory.regions[':c352']
assert(sub and sub.size == 0x80000, 'missing sub ROM region')
assert(wave and wave.size >= 64, 'missing C352 ROM region')
local function putword(region, address, value)
  region:write_u8(address, math.floor(value / 256) % 256)
  region:write_u8(address + 1, value % 256)
end
for i = 0, 63 do wave:write_u8(i, 0x40) end
putword(sub, 0, 0)
putword(sub, 2, 0x0100)
local p = 0x100
local function emit(value) putword(sub, p, value); p = p + 2 end
local function movstore(value, address)
  emit(0x7900); emit(value)
  emit(0x6ba0); emit(math.floor(address / 65536)); emit(address % 65536)
end
local function nops(count)
  for _ = 1, count do emit(0x0000) end
end
movstore(0xffff, 0x280000)
movstore(0xffff, 0x280004)
movstore(0x4006, 0x280006)
movstore(0x0000, 0x28000a)
movstore(0x003f, 0x28000c)
nops(keyon_delay_nops)
movstore(0x0000, 0x280404)
nops(192)
movstore(0xa006, 0x280006) -- preserve BUSY until execute-key handles key-off
movstore(0x0000, 0x280404)
emit(0x0180)

local subcpu = m.devices[':sub']
local subspace = subcpu.spaces['program']
local function now_parts()
  local t = m.time
  return t.seconds, t.attoseconds
end
local reset_events = 0
emu.add_machine_reset_notifier(function()
  reset_events = reset_events + 1
  local sec, attos = now_parts()
  print(string.format('C352_RESET_EVENT n=%d sec=%d attos=%d pc=%s', reset_events, sec, attos, tostring(subcpu.state.PC)))
end)
local request_sent = false
local reset_request_sec, reset_request_attos, reset_flags = -1, -1, -1
emu.register_frame_done(function()
  if not request_sent then
    local sec, attos = now_parts()
    if sec > 0 or attos > 1000000000000000 then
      reset_request_sec, reset_request_attos = sec, attos
      reset_flags = subspace:read_u16(0x280006)
      request_sent = true
      print(string.format('C352_RESET_REQUEST sec=%d attos=%d flags=%04x framepc=%s', sec, attos, reset_flags, tostring(subcpu.state.PC)))
      m:soft_reset()
    end
  end
end, 'frame')

local hook = false
for _, sound in pairs(m.sounds) do
  if sound.device.tag == ':c352' then sound.hook = true; hook = true end
end
assert(hook, 'C352 sound interface hook unavailable')
local left, right = {}, {}
local captured = 0
local reported = false
emu.register_sound_update(function(data)
  if reported then return end
  local channels = data[':c352']
  if channels and channels[1] then
    for i = 1, #channels[1] do
      if captured < 20000 then
        left[captured + 1], right[captured + 1] = channels[1][i], channels[2][i]
      end
      captured = captured + 1
    end
  end
  if captured >= 1000 then
    local bursts, in_burst, start_index = {}, false, 0
    for i = 1, math.min(captured, 20000) do
      local audible = left[i] and (left[i] ~= 0 or right[i] ~= 0)
      if audible and not in_burst then
        start_index = i - 1
        in_burst = true
      elseif not audible and in_burst then
        bursts[#bursts + 1] = start_index
        in_burst = false
      end
    end
    if in_burst then bursts[#bursts + 1] = start_index end
    if request_sent and #bursts >= 2 then
      reported = true
      local sec, attos = now_parts()
      local snippets = {}
      for _, index in ipairs(bursts) do
        local samples = {}
        for i = index, math.min(index + 4, 19999) do
          samples[#samples + 1] = string.format('%.9g', left[i + 1] or 0)
        end
        snippets[#snippets + 1] = string.format('%d[%s]', index, table.concat(samples, ':'))
      end
      print(string.format('C352_RESET_PCM captured=%d reset_events=%d request=%d:%d callback=%d:%d flags=%04x bursts=%s',
        captured, reset_events, reset_request_sec, reset_request_attos, sec, attos, subspace:read_u16(0x280006), table.concat(snippets, ',')))
      m:exit()
    end
  end
end)
m:soft_reset()
