#include "kernel_abi.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define BGA_INDEX_PORT  0x01CE
#define BGA_DATA_PORT   0x01CF

#define BGA_INDEX_ID     0x00
#define BGA_INDEX_XRES   0x01
#define BGA_INDEX_YRES   0x02
#define BGA_INDEX_BPP    0x03
#define BGA_INDEX_ENABLE 0x04

#define BGA_DISABLED     0x00
#define BGA_ENABLED      0x01
#define BGA_LFB_ENABLED  0x40

static uint32_t *g_vbe_fb = NULL;
static uint32_t  g_vbe_w  = 0;
static uint32_t  g_vbe_h  = 0;

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (1U << 31) |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    ((uint32_t)offset & 0xFCU);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static uint64_t find_bga_bar0(void) {
    for (uint8_t bus = 0; bus < 4; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32(bus, slot, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFFU);
            uint16_t device = (uint16_t)((id >> 16) & 0xFFFFU);
            if (vendor == 0x1234U && device == 0x1111U) {
                uint32_t bar0 = pci_read32(bus, slot, 0, 0x10);
                uint64_t phys = (uint64_t)(bar0 & ~0xFULL);
                if (phys != 0) return phys;
            }
        }
    }
    return 0xFD000000ULL;
}

static void bga_write(uint16_t reg, uint16_t val) {
    outw(BGA_INDEX_PORT, reg);
    outw(BGA_DATA_PORT, val);
}

static uint16_t bga_read(uint16_t reg) {
    outw(BGA_INDEX_PORT, reg);
    return inw(BGA_DATA_PORT);
}

int vbe_init(uint32_t width, uint32_t height, uint32_t bpp) {
    uint16_t ver = bga_read(BGA_INDEX_ID);
    if ((ver & 0xFFF0U) != 0xB0C0U) {
        return -1;
    }

    uint64_t bar0 = find_bga_bar0();
    bga_write(BGA_INDEX_ENABLE, BGA_DISABLED);
    bga_write(BGA_INDEX_XRES, (uint16_t)width);
    bga_write(BGA_INDEX_YRES, (uint16_t)height);
    bga_write(BGA_INDEX_BPP, (uint16_t)bpp);
    bga_write(BGA_INDEX_ENABLE, BGA_ENABLED | BGA_LFB_ENABLED);

    g_vbe_w  = width;
    g_vbe_h  = height;
    g_vbe_fb = (uint32_t *)(uintptr_t)bar0;
    return 0;
}

uint32_t *vbe_framebuffer(void) {
    return g_vbe_fb;
}

uint32_t vbe_width(void) {
    return g_vbe_w;
}

uint32_t vbe_height(void) {
    return g_vbe_h;
}

uint32_t vbe_pitch_pixels(void) {
    return g_vbe_w;
}

