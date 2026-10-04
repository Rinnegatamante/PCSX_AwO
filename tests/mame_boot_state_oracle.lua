local machine = manager.machine
local cpu = machine.devices[":maincpu"]
local memory = cpu.spaces["program"]
local regs = cpu.state
local elapsed = 0
local function words(label, base, count)
    local values = {}
    for i = 0, count - 1 do
        values[#values + 1] = string.format("%08x", memory:read_u32(base + i * 4))
    end
    print(string.format("%s base=%08x words=%s", label, base, table.concat(values, " ")))
end
print(string.format("BOOT_STATE start_time=%s", tostring(machine.time)))
for _, seconds in ipairs({0.001, 0.01, 0.1, 1}) do
    emu.wait(seconds - elapsed)
    elapsed = seconds
    print(string.format("MILESTONE requested=%.6f actual=%s pc=%08x SR=%08x Cause=%08x EPC=%08x", seconds, tostring(machine.time), regs["pc"].value, regs["SR"].value, regs["Cause"].value, regs["EPC"].value))
    print(string.format("REGS sp=%08x ra=%08x a0=%08x a1=%08x t0=%08x t1=%08x v0=%08x v1=%08x", regs["sp"].value, regs["ra"].value, regs["a0"].value, regs["a1"].value, regs["t0"].value, regs["t1"].value, regs["v0"].value, regs["v1"].value))
    print(string.format("IO exp_base=%08x DMA5_MADR=%08x DMA5_BCR=%08x DMA5_CHCR=%08x DPCR=%08x DICR=%08x", memory:read_u32(0x1f801000), memory:read_u32(0x1f8010d0), memory:read_u32(0x1f8010d4), memory:read_u32(0x1f8010d8), memory:read_u32(0x1f8010f0), memory:read_u32(0x1f8010f4)))
    words("RAM", 0x80000000, 32)
    words("RAM_EXCEPTION", 0x80000080, 16)
    words("RAM_BOOT_HACK", 0x80010000, 8)
    words("SHARED", 0x1f080000, 32)
    words("ROM_RESET", 0xbfc00000, 8)
end
machine:exit()
