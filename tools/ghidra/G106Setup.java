// Pre-analysis setup for the stock G106 app (raw binary at 0x08020000).
// Arg: directory with optional stock_sram_live.bin / stock_ccm_live.bin (SWD snapshots).
//@category G106
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.data.*;

import java.io.File;
import java.io.FileInputStream;
import java.util.LinkedHashMap;
import java.util.Map;

public class G106Setup extends GhidraScript {

    // RAM block from a live SWD snapshot of the running stock app; uninitialized if there is none.
    private MemoryBlock fileBlock(String name, long base, long size, String path) throws Exception {
        File f = new File(path);
        if (!f.isFile()) {
            println(path + " not found: " + name + " left uninitialized");
            MemoryBlock b = currentProgram.getMemory().createUninitializedBlock(name, toAddr(base), size, false);
            b.setRead(true);
            b.setWrite(true);
            return b;
        }
        try (FileInputStream in = new FileInputStream(f)) {
            MemoryBlock b = currentProgram.getMemory().createInitializedBlock(
                name, toAddr(base), in, f.length(), monitor, false);
            b.setRead(true);
            b.setWrite(true);
            b.setExecute(false);
            return b;
        }
    }

    private void ioBlock(String name, long base, long size) throws Exception {
        MemoryBlock b = currentProgram.getMemory().createUninitializedBlock(name, toAddr(base), size, false);
        b.setRead(true);
        b.setWrite(true);
        b.setVolatile(true);
    }

    private void label(long addr, String name) throws Exception {
        createLabel(toAddr(addr), name, true, SourceType.USER_DEFINED);
    }

    private void func(long addr, String name) throws Exception {
        Address a = toAddr(addr & ~1L);
        disassemble(a);
        if (getFunctionAt(a) == null)
            createFunction(a, name);
        else if (name != null)
            getFunctionAt(a).setName(name, SourceType.USER_DEFINED);
    }

    @Override
    public void run() throws Exception {
        String dir = getScriptArgs()[0];

        // Flash is read-only: lets the decompiler fold literal-pool constants.
        MemoryBlock flash = currentProgram.getMemory().getBlock(toAddr(0x08020000L));
        flash.setName("FLASH");
        flash.setWrite(false);
        flash.setExecute(true);

        fileBlock("SRAM", 0x20000000L, 0x20000, dir + "/stock_sram_live.bin");
        fileBlock("CCM", 0x10000000L, 0x10000, dir + "/stock_ccm_live.bin");
        ioBlock("APB1", 0x40000000L, 0x8000);
        ioBlock("APB2", 0x40010000L, 0x8000);
        ioBlock("AHB1", 0x40020000L, 0x60000);
        ioBlock("SCS", 0xE000E000L, 0x1000);

        Map<Long, String> periph = new LinkedHashMap<>();
        periph.put(0x40000000L, "TIM2");     periph.put(0x40003400L, "I2S2ext");
        periph.put(0x40003800L, "SPI2_I2S2"); periph.put(0x40005400L, "I2C1");
        periph.put(0x40005800L, "I2C2");     periph.put(0x40007000L, "PWR");
        periph.put(0x40007400L, "DAC");      periph.put(0x40010000L, "TIM1");
        periph.put(0x40011000L, "USART1");   periph.put(0x40012000L, "ADC1");
        periph.put(0x40012300L, "ADC_COMMON"); periph.put(0x40013000L, "SPI1");
        periph.put(0x40013800L, "SYSCFG");   periph.put(0x40013C00L, "EXTI");
        periph.put(0x40020000L, "GPIOA");    periph.put(0x40020400L, "GPIOB");
        periph.put(0x40020800L, "GPIOC");    periph.put(0x40020C00L, "GPIOD");
        periph.put(0x40021C00L, "GPIOH");    periph.put(0x40023000L, "CRC");
        periph.put(0x40023800L, "RCC");      periph.put(0x40023C00L, "FLASH_IF");
        periph.put(0x40026000L, "DMA1");     periph.put(0x40026400L, "DMA2");
        for (Map.Entry<Long, String> e : periph.entrySet())
            label(e.getKey(), e.getValue());

        // Vector table: 16 core + 82 IRQ entries.
        String[] core = {"_estack", "Reset_Handler", "NMI_Handler", "HardFault_Handler",
            "MemManage_Handler", "BusFault_Handler", "UsageFault_Handler", null, null, null, null,
            "SVC_Handler", "DebugMon_Handler", null, "PendSV_Handler", "SysTick_Handler"};
        Memory mem = currentProgram.getMemory();
        for (int i = 0; i < 16 + 82; i++) {
            Address slot = toAddr(0x08020000L + 4L * i);
            createData(slot, PointerDataType.dataType);
            if (i == 0)
                continue;
            long v = mem.getInt(slot) & 0xFFFFFFFFL;
            if (v < 0x08020000L || v >= 0x08100000L)
                continue;
            String name = i < 16 ? core[i] : "IRQ" + (i - 16) + "_Handler";
            if (name == null)
                continue;
            if (getFunctionAt(toAddr(v & ~1L)) == null)
                func(v, name);
        }
        // Distinct names for the IRQs the stock app actually enables.
        nameVector(16 + 14, "DMA1_Stream3_IRQHandler_audio");
        nameVector(16 + 25, "TIM1_UP_TIM10_IRQHandler");
        nameVector(16 + 28, "TIM2_IRQHandler_tick");
        nameVector(16 + 37, "USART1_IRQHandler");
        nameVector(16 + 56, "DMA2_Stream0_IRQHandler");

        // Known functions (see g106_open/README.md).
        func(0x08025f78L, "SystemInit");
        func(0x08020248L, "__main");
        func(0x08021ac4L, "__scatterload");
        func(0x080305a4L, "main");
        func(0x08022f54L, "GPIO_Init");
        func(0x08022feeL, "GPIO_PinAFConfig");
        func(0x0802301eL, "GPIO_ResetBits");
        func(0x08023022L, "GPIO_SetBits");
        func(0x08023026L, "GPIO_WriteBit");
        func(0x08025c88L, "RCC_AHB1PeriphClockCmd");
        func(0x08025cd0L, "RCC_APB2PeriphClockCmd");
        func(0x0802724aL, "delay_ms");
        func(0x08030eacL, "i2c_read");
        func(0x08030fccL, "i2c_write");
        func(0x08024a42L, "I2C_Send7bitAddress");
        func(0x08024a52L, "I2C_SendData");
        func(0x08024928L, "I2C_GenerateSTART");
        func(0x08030e80L, "I2C_WaitEvent");
        func(0x0802c2ccL, "aic3204_set_page");
        func(0x0802c404L, "aic3204_write");
        func(0x08030890L, "digipot_2E_write");
        func(0x080308dcL, "tea5767_read");
        func(0x0803090cL, "tea5767_init");
        func(0x08030934L, "tea5767_mute");
        func(0x08030964L, "tea5767_set_freq");
        func(0x0802ea30L, "tx_allowed_on_freq");
        func(0x0802664cL, "spi1_display_init");
        func(0x08025abcL, "display_dma_pump");
        func(0x08029d18L, "power_key_task");
        label(0x08038b3cL, "tx_band_table");
        labelPins();
    }

    // Stock keeps GPIO pins as a linked list of objects {prev, next, GPIOx, pinmask, ...}
    // starting at 0x200007a4. Apply a struct and name each one pin_<port><n>.
    private void labelPins() throws Exception {
        StructureDataType pin = new StructureDataType("gpio_pin_t", 0);
        pin.add(PointerDataType.dataType, "prev", null);
        pin.add(PointerDataType.dataType, "next", null);
        pin.add(PointerDataType.dataType, "port", null);
        pin.add(UnsignedShortDataType.dataType, "mask", null);
        pin.add(UnsignedShortDataType.dataType, "pad", null);
        pin.add(UnsignedIntegerDataType.dataType, "cfg", null);
        DataType dt = currentProgram.getDataTypeManager().addDataType(pin, DataTypeConflictHandler.REPLACE_HANDLER);

        Memory mem = currentProgram.getMemory();
        long p = 0x200007a4L;
        for (int n = 0; p != 0 && n < 64; n++) {
            long port = mem.getInt(toAddr(p + 8)) & 0xFFFFFFFFL;
            int mask = mem.getShort(toAddr(p + 12)) & 0xFFFF;
            String name = "pin_P" + (char) ('A' + (int) ((port - 0x40020000L) / 0x400)) + Integer.numberOfTrailingZeros(mask);
            clearListing(toAddr(p), toAddr(p + dt.getLength() - 1));
            createData(toAddr(p), dt);
            label(p, name);
            p = mem.getInt(toAddr(p + 4)) & 0xFFFFFFFFL;
        }
    }

    private void nameVector(int idx, String name) throws Exception {
        long v = currentProgram.getMemory().getInt(toAddr(0x08020000L + 4L * idx)) & 0xFFFFFFFFL;
        if (v >= 0x08020000L && v < 0x08100000L)
            func(v, name);
    }
}
