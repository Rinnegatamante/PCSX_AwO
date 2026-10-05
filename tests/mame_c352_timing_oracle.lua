-- Temporary in-memory C352 timing fixture for MAME's actual C352 device.
-- The Python runner replaces the two marker tokens before launching MAME.
if _G.C352_TIMING_PROBE_DONE then return end
_G.C352_TIMING_PROBE_DONE = true
local keyon_delay_nops = __KEYON_DELAY_NOPS__
local keyoff_delay_nops = 192
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
  emit(0x7900); emit(value)                         -- mov.w #imm16,r0
  emit(0x6ba0); emit(math.floor(address / 65536)); emit(address % 65536) -- mov.w r0,@abs32
end
local function nops(count)
  for _ = 1, count do emit(0x0000) end
end
movstore(0xffff, 0x280000) -- front volume
movstore(0xffff, 0x280004) -- frequency
movstore(0x4006, 0x280006) -- key-on + loop/filter
movstore(0x0000, 0x28000a) -- sample start
movstore(0x003f, 0x28000c) -- sample end
nops(keyon_delay_nops)
movstore(0x0000, 0x280404) -- execute key-on
nops(keyoff_delay_nops)
movstore(0xa006, 0x280006) -- preserve BUSY until execute-key handles key-off
movstore(0x0000, 0x280404) -- execute key-off
emit(0x0180) -- sleep while the sound stream advances

local hook = false
for _, sound in pairs(m.sounds) do
  if sound.device.tag == ':c352' then sound.hook = true; hook = true end
end
assert(hook, 'C352 sound interface hook unavailable')
local samples = {}
local subcpu = m.devices[':sub']
local subspace = subcpu.spaces['program']
local captured = 0
local reported = false
emu.register_sound_update(function(data)
  if reported then return end
  local channels = data[':c352']
  if channels and channels[1] then
    for i = 1, #channels[1] do
      local left, right = channels[1][i], channels[2][i]
      if captured < 128 then samples[captured + 1] = {left, right} end
      captured = captured + 1
    end
  end
  if captured >= 128 then
    reported = true
    local first_nonzero, first_silent_after = -1, -1
    local had_audio = false
    for i = 1, math.min(captured, 128) do
      local frame = samples[i]
      if frame then
        local nonzero = frame[1] ~= 0 or frame[2] ~= 0
        if nonzero then
          had_audio = true
          if first_nonzero < 0 then first_nonzero = i - 1 end
        elseif had_audio and first_silent_after < 0 then
          first_silent_after = i - 1
        end
      end
    end
    local flags = subspace:read_u16(0x280006)
    local out = {}
    for i = 1, math.min(captured, 16) do
      local frame = samples[i]
      out[#out + 1] = frame and string.format('%.9g:%.9g', frame[1], frame[2]) or 'missing'
    end
    print(string.format('C352_ORACLE target=%d samples=%d first_on=%d first_off=%d flags=%04x pcm=%s',
      78 + keyon_delay_nops * 2, captured, first_nonzero, first_silent_after, flags, table.concat(out, ',')))
    m:exit()
  end
end)
m:soft_reset()
