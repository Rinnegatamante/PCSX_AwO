-- Transient H8 peripheral CPU probe. Python substitutes MODE and FIRMWARE.
if _G.H8_PERIPHERALS_DONE then return end
_G.H8_PERIPHERALS_DONE = true

local m = manager.machine
local sub = m.memory.regions[':sub']
local mainrom = m.memory.regions[':maincpu:rom']
local subcpu = m.devices[':sub']
local space = subcpu and subcpu.spaces['program']
assert(sub and sub.size == 0x80000 and mainrom and mainrom.size >= 8 and space,
  'missing System 12 CPU ROMs or H8 program space')

local function putword(region, address, value)
  region:write_u8(address, math.floor(value / 256) % 256)
  region:write_u8(address + 1, value % 256)
end

-- Park the PSX main CPU in its boot ROM while the H8 firmware runs.
mainrom:write_u32(0, 0x0bf00000)
mainrom:write_u32(4, 0)
local firmware = { FIRMWARE }
putword(sub, 0, 0)
putword(sub, 2, 0x0100)
for i, word in ipairs(firmware) do putword(sub, 0x100 + (i - 1) * 2, word) end

local mode = 'MODE'
local function stamp(t)
  return string.format('%d:%018d', t.seconds, t.attoseconds)
end
local reads = 0
local stopped = false
local observation_count = 0
local max_observations = MAXOBS
local taps = {}
local function tapword(address, high_name, low_name)
  local function regname(mem_mask)
    return (mem_mask & 0xff00) ~= 0 and high_name or low_name
  end
  taps[#taps + 1] = space:install_read_tap(address, address + 1, 'h8-periph-' .. high_name,
    function(offset, data, mem_mask)
      local value = (mem_mask & 0xff00) ~= 0 and math.floor(data / 256) % 256 or data % 256
      reads = reads + 1
      local t = m.time
      print(string.format('H8PERIPH mode=%s kind=read reg=%s value=%02x time=%s pc=%s',
        mode, regname(mem_mask), value, stamp(t), tostring(subcpu.state.PC.value)))
    end)
  taps[#taps + 1] = space:install_write_tap(address, address + 1, 'h8-periph-write-' .. high_name,
    function(offset, data, mem_mask)
      local value = (mem_mask & 0xff00) ~= 0 and math.floor(data / 256) % 256 or data % 256
      local t = m.time
      print(string.format('H8PERIPH mode=%s kind=write reg=%s value=%02x time=%s pc=%s',
        mode, regname(mem_mask), value, stamp(t), tostring(subcpu.state.PC.value)))
    end)
end

if mode == 'adc' then
  tapword(0xffffe8, 'ADCSR', 'ADCR')
  tapword(0xffffe0, 'ADDR0H', 'ADDR0L')
elseif mode == 'timer' then
  tapword(0xffff60, 'TSTR', 'TSTR_UNUSED')
  tapword(0xffff64, 'TCR0', 'TIER0')
  tapword(0xffff66, 'TIER0', 'TSR0')
  tapword(0xffff68, 'TCNT0H', 'TCNT0L')
elseif mode == 'sci' or mode == 'sci_external' or mode == 'sci_rx' then
  tapword(0xffffb0, 'SMR0', 'BRR0')
  tapword(0xffffb2, 'SCR0', 'TDR0')
  tapword(0xffffb4, 'SSR0', 'RDR0')
else
  error('unknown H8 peripheral mode ' .. mode)
end
_G.H8_PERIPHERALS_TAPS = taps
taps[#taps + 1] = space:install_write_tap(0x080000, 0x080003, 'h8-periph-observation',
  function(offset, data, mem_mask)
    if observation_count < max_observations then
      local value = (mem_mask & 0xff00) ~= 0 and math.floor(data / 256) % 256 or data % 256
      local address = offset + ((mem_mask & 0xff00) ~= 0 and 0 or 1)
      observation_count = observation_count + 1
      print(string.format('H8OBS mode=%s address=%06x value=%02x time=%s pc=%s', mode,
        address, value, stamp(m.time), tostring(subcpu.state.PC.value)))
      if observation_count == max_observations and not stopped then
        stopped = true
        m:exit()
      end
    end
  end)

local reset_count = 0
emu.add_machine_reset_notifier(function()
  reset_count = reset_count + 1
  print(string.format('H8PERIPH_RESET mode=%s n=%d time=%s pc=%s', mode, reset_count,
    stamp(m.time), tostring(subcpu.state.PC.value)))
end)
m:soft_reset()
