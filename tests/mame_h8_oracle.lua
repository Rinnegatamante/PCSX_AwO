local machine = manager.machine
local sub = machine.devices[":sub"]
local main = machine.devices[":maincpu"]
local memory = sub.spaces["program"]
local regs = sub.state
local elapsed = 0
print("H8_ORACLE_REGISTERS")
for name, entry in pairs(regs) do print(string.format("H8_REGISTER %s=%x", name, entry.value)) end
local function words(label, space, base, count)
    local values = {}
    for i = 0, count - 1 do values[#values + 1] = string.format("%04x", space:read_u16(base+i*2)) end
    print(string.format("%s base=%06x words=%s", label, base, table.concat(values," ")))
end
local function bytes(label,base,count)
    local values={}
    for i=0,count-1 do values[#values+1]=string.format("%02x",memory:read_u8(base+i)) end
    print(string.format("%s base=%06x bytes=%s",label,base,table.concat(values," ")))
end
for _, seconds in ipairs({0.1,1,2,10,20}) do
    emu.wait(seconds-elapsed)
    elapsed=seconds
    print(string.format("H8_MILESTONE requested=%.6f actual=%s pc=%06x CCR=%02x",seconds,tostring(machine.time),regs["PC"].value,regs["CCR"].value))
    print(string.format("PSX_MILESTONE pc=%08x SR=%08x Cause=%08x EPC=%08x",main.state["pc"].value,main.state["SR"].value,main.state["Cause"].value,main.state["EPC"].value))
    local values={}
    for i=0,7 do values[#values+1]=string.format("ER%d=%08x",i,(regs["R"..i].value| (regs["E"..i].value << 16))) end
    print("H8_REGS "..table.concat(values," "))
    bytes("H8_SCI",0xffffb0,16)
    bytes("H8_IRQ",0xfffff2,8)
    bytes("H8_TIMER",0xffff60,64)
    words("H8_SHARED",memory,0x080400,16)
    words("H8_STACK",memory,0x087fe0,16)
    words("H8_SHARED_3078",memory,0x083078,16)
    words("H8_SHARED_3140",memory,0x083140,16)
    words("PSX_SHARED_3078",main.spaces["program"],0x1f083078,16)
    words("PSX_SHARED_3140",main.spaces["program"],0x1f083140,16)
    words("PSX_SHARED",main.spaces["program"],0x1f080400,16)
end
print("H8_ORACLE_PASS")
machine:exit()
