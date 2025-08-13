#define accnet_reg_read64(base, reg) (((volatile uint64_t *)(base))[(reg)/8])
#define accnet_reg_write64(base, reg, val) (((volatile uint64_t *)(base))[(reg)/8]) = val

#define accnet_reg_read32(base, reg) (((volatile uint32_t *)(base))[(reg)/4])
#define accnet_reg_write32(base, reg, val) (((volatile uint32_t *)(base))[(reg)/4]) = val

#define accnet_reg_read16(base, reg) (((volatile uint16_t *)(base))[(reg)/2])
#define accnet_reg_write16(base, reg, val) (((volatile uint16_t *)(base))[(reg)/2]) = val

#define accnet_reg_read8(base, reg) (((volatile uint8_t *)(base))[(reg)/1])
#define accnet_reg_write8(base, reg, val) (((volatile uint8_t *)(base))[(reg)/1]) = val

#define MAP_INDEX(idx) (((uint64_t) idx) << 40)