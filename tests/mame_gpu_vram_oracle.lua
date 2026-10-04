-- Type-2 2 MiB VRAM bus oracle. Uses actual GPU MMIO, not private device data.
local machine = manager.machine
local memory = machine.devices[":maincpu"].spaces["program"]
local function gp0(word) memory:write_u32(0x1f801810, word) end
local function upload(x, y, pixel)
    gp0(0xa0000000); gp0(x | (y << 16)); gp0(0x00010001)
    gp0(pixel | (pixel << 16))
end
local function read_pixel(x, y)
    gp0(0xc0000000); gp0(x | (y << 16)); gp0(0x00010002)
    return memory:read_u32(0x1f801810) & 0xffff
end
memory:write_u32(0x1f801814, 0)
gp0(0xe3000000); gp0(0xe4000000 | 1023 | (1023 << 10)); gp0(0xe5000000)
upload(0, 512, 0x0001); upload(1, 800, 0x7c00)
gp0(0xe1000800)
gp0(0x64808080); gp0(100 | (600 << 16)); gp0((800 * 64) << 16); gp0(0x00010001)
local pixel = read_pixel(100, 600)
assert(pixel == 0x7c00, string.format("upper 4bpp pixel %04x != 7c00", pixel))
gp0(0xe1000880)
gp0(0x64808080); gp0(101 | (600 << 16)); gp0((800 * 64) << 16); gp0(0x00010001)
assert(read_pixel(101, 600) == 0x7c00, "upper 8bpp fetch failed")
upload(64, 512, 0x0001)
gp0(0xe1000881)
gp0(0x64808080); gp0(102 | (600 << 16)); gp0((800 * 64) << 16); gp0(0x00010001)
assert(read_pixel(102, 600) == 0x7c00, "upper odd 8bpp fetch failed")
upload(0, 512, 0x03e0)
gp0(0xe1000900)
gp0(0x64808080); gp0(103 | (600 << 16)); gp0(0); gp0(0x00010001)
assert(read_pixel(103, 600) == 0x03e0, "upper 16bpp fetch failed")
assert(read_pixel(0, 0) == 0, "upper VRAM unexpectedly aliases console row zero")
print(string.format("GPU_VRAM_ORACLE_PASS rows=1024 upper_tpage=0800 clut_y=800 dst_y=600 pixel=%04x modes=4bpp/8bpp/odd8bpp/16bpp", pixel))
machine:exit()
