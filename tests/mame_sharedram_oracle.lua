local machine = manager.machine
local main = machine.devices[":maincpu"].spaces["program"]
local sub = machine.devices[":sub"].spaces["program"]
local function dump(label)
    print(label)
    for _, address in ipairs({0x1f080000, 0x1f080004, 0x1f080008, 0x1f08000c}) do
        print(string.format("MAIN32 [%08x]=%08x MAIN16lo=%04x MAIN16hi=%04x", address, main:read_u32(address), main:read_u16(address), main:read_u16(address+2)))
    end
    for i = 0, 7 do
        print(string.format("SUB16 [%08x]=%04x", 0x080000+i*2, sub:read_u16(0x080000+i*2)))
    end
end
print("SHARED_MAP start=" .. tostring(machine.time))
for i = 0, 31 do sub:write_u8(0x080000+i, 0) end
main:write_u32(0x1f080000, 0x76543210)
dump("STEP write_u32 MAIN 1f080000=76543210")
assert(main:read_u32(0x1f080000) == 0x76543210)
assert(sub:read_u16(0x080000) == 0x3210)
assert(sub:read_u16(0x080002) == 0x7654)
assert(sub:read_u16(0x080004) == 0)
assert(sub:read_u16(0x080006) == 0)
main:write_u8(0x1f080001, 0xab)
dump("STEP write_u8 MAIN 1f080001=ab")
assert(main:read_u32(0x1f080000) == 0x7654ab10)
assert(sub:read_u16(0x080000) == 0xab10)
assert(sub:read_u16(0x080002) == 0x7654)
main:write_u16(0x1f080002, 0xcdef)
dump("STEP write_u16 MAIN 1f080002=cdef")
assert(main:read_u32(0x1f080000) == 0xcdefab10)
assert(sub:read_u16(0x080002) == 0xcdef)
main:write_u16(0x1f080004, 0x1357)
dump("STEP write_u16 MAIN 1f080004=1357")
assert(main:read_u32(0x1f080004) == 0x00001357)
assert(sub:read_u16(0x080004) == 0x1357)
sub:write_u16(0x080006, 0x2468)
dump("STEP write_u16 SUB 080006=2468")
assert(main:read_u32(0x1f080004) == 0x24681357)
print("SHARED_MAP_ORACLE_PASS stride=2 both_cpu32_lanes=true")
machine:exit()
