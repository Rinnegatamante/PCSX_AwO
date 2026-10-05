-- Real H8 SCI firmware and passive global emu_timer save-item observations.
if _G.H8_SYNC_TIMER_PROBE then return end
_G.H8_SYNC_TIMER_PROBE = true
local m = manager.machine
local sub, main = m.memory.regions[':sub'], m.memory.regions[':maincpu:rom']
local cpu = m.devices[':sub']
local space = cpu.spaces['program']
local sci = m.devices[':sub:sciCHANNEL']
assert(sub and main and sci, 'missing H8/SCI ROM or device')
main:write_u32(0, 0x0bf00000)
main:write_u32(4, 0)
local function word(address, value)
  sub:write_u8(address, math.floor(value / 256) % 256)
  sub:write_u8(address + 1, value % 256)
end
word(0, 0); word(2, 0x100)
local firmware = { INJECT_WORDS }
local loaded = {}
for i, value in ipairs(firmware) do
  local address = 0x100 + (i - 1) * 2
  word(address, value)
  loaded[#loaded+1] = string.format('%04x', sub:read_u8(address)*256 + sub:read_u8(address+1))
end
print('H8SYNC_FIRMWARE words=' .. table.concat(loaded, ','))

-- Lua exposes unnamed global save items by index. Discover the emu_timer
-- register_save layout (schedule.cpp/save.h), never assume a binary index.
-- Sorted fields: enabled, expire AS/sec, index, param, period AS/sec, start AS/sec.
local metadata, groups = {}, {}
for index=0,30000 do
  local item = emu.item(index)
  if item.size == 0 then break end
  metadata[#metadata+1] = { index=index, size=item.size, count=item.count }
end
assert(#metadata < 30001, 'save registry discovery limit exceeded')
local pattern = {1,8,4,4,4,8,4,8,4}
for i=1,#metadata-8 do
  local match = true
  for j=1,9 do
    if metadata[i+j-1].count ~= 1 or metadata[i+j-1].size ~= pattern[j] then match=false end
  end
  if match then groups[#groups+1] = metadata[i].index end
end
assert(#groups > 0, 'no emu_timer save-item layout found')
local function state(index)
  local values = {}
  for j=0,8 do values[j+1] = emu.item(index+j):read(0) end
  return values
end
local timer_index
local function identify()
  local candidates = {}
  for _, index in ipairs(groups) do
    local v = state(index)
    -- The injected SCI alone arms a finite one-shot at a nonzero local time
    -- before the first marker; cold screen/RTC timers have start time zero.
    if v[1] == 1 and v[3] == 0 and v[7] == 1000000000
      and v[8] > 0 and v[9] == 0 then candidates[#candidates+1] = index end
  end
  assert(#candidates == 1, 'SCI timer identification must be unique')
  timer_index = candidates[1]
  print(string.format('H8SYNC_IDENT index=%d groups=%d registry_items=%d',
    timer_index, #groups, #metadata))
end
local clock_index = assert(sci.items['0/m_clock_event'], 'missing named SCI clock_event')
local clock_item = emu.item(clock_index)
local observed = 0
local tap = space:install_write_tap(0x080000, 0x080001, 'h8-sync-timer-marker',
  function(offset, data, mask)
    if observed >= MAXOBS then return end
    if not timer_index then identify() end
    observed = observed + 1
    local v, t = state(timer_index), m.time
    print(string.format('H8SYNC_OBS n=%d time=%d:%018d armed=%d expire=%d:%018d start=%d:%018d clock_event=%d',
      observed, t.seconds, t.attoseconds, v[1], v[3], v[2], v[9], v[8], clock_item:read(0)))
    if observed == MAXOBS then m:exit() end
  end)
_G.H8_SYNC_TIMER_TAP = tap
local resets = 0
emu.add_machine_reset_notifier(function()
  resets = resets + 1
  print(string.format('H8SYNC_RESET n=%d time=%d:%018d', resets, m.time.seconds, m.time.attoseconds))
end)
m:soft_reset()
