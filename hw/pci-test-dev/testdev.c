#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"

#define MMIO_LENGTH (1 << 20)

#define MAX_VECTOR_COUNT 3
#define PAGE_SIZE 4096
#define PAGE_MASK (PAGE_SIZE - 1)
#define PAGE_SHIFT 12

#define CHAIN_BIT 0x0001

typedef struct SgEntry {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
} SgEntry;

typedef struct IrqStatusReg {
    union {
        struct {
            uint32_t timer_irq       : 1;
            uint32_t vec_add_done    : 1;
            uint32_t bitflip_done    : 1;
            uint32_t reserved        : 29;
        } fields;
        uint32_t value;
    };
} IrqStatusReg;

typedef struct VectorAddCtrlReg {
    union {
        struct {
            uint32_t doorbell      : 1;
            uint32_t elem_cnt      : 31;
        } fields;
        uint32_t value;
    };
} VectorAddCtrlReg;

typedef struct BitFlipCtrlReg {
    union {
        struct {
            uint32_t doorbell      : 1;
            uint32_t sgl_size      : 31;
        } fields;
        uint32_t value;
    };
} BitFlipCtrlReg;

typedef struct PciTestDev {
    PCIDevice parent;
    MemoryRegion mmio;
    uint8_t *mmio_regs;
    QEMUTimer timer;
    uint8_t timer_enabled;
    uint32_t counter;
    IrqStatusReg irq_status;
    VectorAddCtrlReg vec_add_ctrl;
    uint64_t vector_addr[MAX_VECTOR_COUNT];
    void *internal_buffer[MAX_VECTOR_COUNT];
    QEMUTimer vector_add_timer;
    BitFlipCtrlReg bitflip_ctrl;
    uint64_t bitflip_sgl;
    void *bitflip_data_buffer;
    void *bitflip_sgl_buffer;
    QEMUTimer bitflip_timer;
} PciTestDev;

#define TYPE_PCI_TEST_DEV "pci-test-dev"
#define PCI_TEST_DEV(obj) OBJECT_CHECK(PciTestDev, (obj), TYPE_PCI_TEST_DEV)

#define REG_ENABLE_TIMER      0x0
#define REG_COUNTER           0x4
#define REG_IRQ_STATUS        0x10
#define REG_VECTOR_ADD_CTRL   0x20
#define REG_VECTOR1_ADDR_LO   0x24
#define REG_VECTOR1_ADDR_HI   0x28
#define REG_VECTOR2_ADDR_LO   0x2C
#define REG_VECTOR2_ADDR_HI   0x30
#define REG_VECTOR3_ADDR_LO   0x34
#define REG_VECTOR3_ADDR_HI   0x38
#define REG_BITFLIP_CTRL      0x40
#define REG_BITFLIP_SGL_LO    0x44
#define REG_BITFLIP_SGL_HI    0x48

static void timer_callback(void *param)
{
    PciTestDev *pci_test_dev = param;

    info_report("[TEST] %s, pci_test_dev=%p",
        __func__, pci_test_dev);

    if (msi_enabled(&pci_test_dev->parent))
    {
        info_report("[TEST] %s, pci_test_dev=%p, msi notify",
            __func__, pci_test_dev);
        msi_notify(&pci_test_dev->parent, 0);
    }

    if (pci_test_dev->timer_enabled)
    {
        pci_test_dev->counter++;
        timer_mod(&pci_test_dev->timer,
            qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    }
}

static void create_timer(PciTestDev *pci_test_dev)
{
    pci_test_dev->timer_enabled = 0;
    pci_test_dev->counter = 0;
    timer_init_ms(&pci_test_dev->timer, QEMU_CLOCK_VIRTUAL,
        timer_callback, pci_test_dev);
}

static void reset_timer(PciTestDev *pci_test_dev)
{
    pci_test_dev->timer_enabled = 0;
    pci_test_dev->counter = 0;
}

static void destroy_timer(PciTestDev *pci_test_dev)
{
    pci_test_dev->timer_enabled = 0;
    pci_test_dev->counter = 0;
    timer_del(&pci_test_dev->timer);
}

static void vector_add(void *vec1, void *vec2, void *vec3,
    size_t size)
{
    uint64_t *v1 = vec1;
    uint64_t *v2 = vec2;
    uint64_t *v3 = vec3;
    for (size_t i = 0; i < size / sizeof(uint64_t); i++) {
        v3[i] = v1[i] + v2[i];
    }
}

static void vector_add_callback(void *param)
{
    PciTestDev *pci_test_dev = param;
    dma_addr_t dma_addr[MAX_VECTOR_COUNT];
    void *vec_buf[MAX_VECTOR_COUNT];
    dma_addr_t remaining = 0, offset = 0, copy_len = 0;
    uint32_t elem_cnt = 0;
    MemTxResult result = MEMTX_OK;

    info_report("[TEST] %s, pci_test_dev=%p",
        __func__, pci_test_dev);

    for (int i = 0; i < MAX_VECTOR_COUNT; i++) {
        dma_addr[i] = pci_test_dev->vector_addr[i];
        if (dma_addr[i] == 0) {
            info_report("[TEST][ERROR] %s, vector dma "
                "address %d is zero", __func__, i);
            return;
        }
        vec_buf[i] = pci_test_dev->internal_buffer[i];
        if (vec_buf[i] == NULL) {
            info_report("[TEST][ERROR] %s, internal "
                "buffer %d is NULL", __func__, i);
            return;
        }
    }

    elem_cnt = pci_test_dev->vec_add_ctrl.fields.elem_cnt;
    remaining = elem_cnt * sizeof(uint64_t);

    do
    {
        copy_len = (remaining > PAGE_SIZE) ?
            PAGE_SIZE : remaining;
        remaining -= copy_len;
        result = pci_dma_read(&pci_test_dev->parent,
            dma_addr[0] + offset, vec_buf[0], copy_len);
        if (result != MEMTX_OK) {
            info_report("[TEST][ERROR] %s, pci_dma_read "
                "failed, result: %d", __func__, result);
            return;
        }

        result = pci_dma_read(&pci_test_dev->parent,
            dma_addr[1] + offset, vec_buf[1], copy_len);
        if (result != MEMTX_OK) {
            info_report("[TEST][ERROR] %s, pci_dma_read "
                "failed, result: %d", __func__, result);
            return;
        }

        vector_add(vec_buf[0], vec_buf[1], vec_buf[2],
            copy_len);

        result = pci_dma_write(&pci_test_dev->parent,
            dma_addr[2] + offset, vec_buf[2], copy_len);
        if (result != MEMTX_OK) {
            info_report("[TEST][ERROR] %s, pci_dma_write "
                "failed, result: %d", __func__, result);
            return;
        }
        offset += copy_len;
    } while (remaining > 0);

    pci_test_dev->irq_status.fields.vec_add_done = 1;
    msi_notify(&pci_test_dev->parent, 0);
}

static void init_vector_add(PciTestDev *pci_test_dev)
{
    pci_test_dev->vec_add_ctrl.value = 0;

    for (int i = 0; i < MAX_VECTOR_COUNT; i++) {
        pci_test_dev->vector_addr[i] = 0;
    }

    for (int i = 0; i < MAX_VECTOR_COUNT; i++) {
        pci_test_dev->internal_buffer[i] =
            aligned_alloc(64, PAGE_SIZE);
        if (pci_test_dev->internal_buffer[i] == NULL) {
            info_report("[TEST][ERROR] %s, allocate "
                "internal buffer %d failed", __func__, i);
            return;
        }
        memset(pci_test_dev->internal_buffer[i], 0,
            PAGE_SIZE);
    }

    timer_init_ms(&pci_test_dev->vector_add_timer,
        QEMU_CLOCK_VIRTUAL, vector_add_callback,
        pci_test_dev);
}

static void reset_vector_add(PciTestDev *pci_test_dev)
{
    pci_test_dev->vec_add_ctrl.value = 0;

    for (int i = 0; i < MAX_VECTOR_COUNT; i++) {
        pci_test_dev->vector_addr[i] = 0;
    }

    for (int i = 0; i < MAX_VECTOR_COUNT; i++) {
        if (pci_test_dev->internal_buffer[i] != NULL) {
            memset(pci_test_dev->internal_buffer[i], 0,
                PAGE_SIZE);
        }
    }
}

static void deinit_vector_add(PciTestDev *pci_test_dev)
{
    pci_test_dev->vec_add_ctrl.value = 0;

    for (int i = 0; i < MAX_VECTOR_COUNT; i++) {
        pci_test_dev->vector_addr[i] = 0;
    }

    for (int i = 0; i < MAX_VECTOR_COUNT; i++) {
        if (pci_test_dev->internal_buffer[i] != NULL) {
            free(pci_test_dev->internal_buffer[i]);
            pci_test_dev->internal_buffer[i] = 0;
        }
    }

    timer_del(&pci_test_dev->vector_add_timer);
}

static int handle_bitflip_sge(PciTestDev *pdev, SgEntry sge)
{
    int ret = 0;
    MemTxResult result = MEMTX_OK;
    dma_addr_t data_addr = sge.address;
    uint8_t *buffer = pdev->bitflip_data_buffer;

    if (data_addr == 0) {
        info_report("[TEST][ERROR] %s, data "
            "address is zero", __func__);
        ret = -1;
        goto done;
    }

    uint32_t remaining = sge.length;
    uint32_t offset = 0;
    while (remaining)
    {
        uint32_t copy_len = (remaining > PAGE_SIZE) ?
            PAGE_SIZE : remaining;
        result = pci_dma_read(&pdev->parent,
            data_addr + offset, buffer, copy_len);
        if (result != MEMTX_OK) {
            info_report("[TEST][ERROR] %s, dma read "
                "data failed, result: %d", __func__,
                result);
            ret = -1;
            goto done;
        }

        // Bitwise NOT operation
        for (uint32_t i = 0; i < copy_len; i++) {
            buffer[i] = ~buffer[i];
        }

        result = pci_dma_write(&pdev->parent,
            data_addr + offset, buffer, copy_len);
        if (result != MEMTX_OK) {
            info_report("[TEST][ERROR] %s, dma write "
                "data failed, result: %d", __func__,
                result);
            ret = -1;
            goto done;
        }
        remaining -= copy_len;
        offset += copy_len;
    }
done:
    return ret;
}

static void bitflip_timer_callback(void *param)
{
    PciTestDev *pdev = param;
    dma_addr_t sgl = 0;
    uint32_t sgl_size = 0;
    SgEntry *sgl_buf = NULL;
    MemTxResult result = MEMTX_OK;

    sgl = pdev->bitflip_sgl;
    if (sgl == 0) {
        info_report("[TEST][ERROR] %s, bitflip sg "
            "list is 0", __func__);
        return;
    }

    sgl_buf = pdev->bitflip_sgl_buffer;
    sgl_size = pdev->bitflip_ctrl.fields.sgl_size;
    do {
        if (sgl_size > PAGE_SIZE) {
            info_report("[TEST][ERROR] %s, bitflip sg "
                "list size %d is too large", __func__,
                sgl_size);
            return;
        }

        result = pci_dma_read(&pdev->parent, sgl,
            sgl_buf, sgl_size);
        if (result != MEMTX_OK) {
            info_report("[TEST][ERROR] %s, dma read sgl "
                "failed, result: %d", __func__, result);
            return;
        }

        uint32_t num_sge = sgl_size / sizeof(SgEntry);
        for (uint32_t i = 0; i < num_sge; i++) {
            info_report("[TEST] %s, sgl[%03d]: "
                "address=0x%016lx, length=%u, flags=0x%x",
                __func__, i, sgl_buf[i].address,
                sgl_buf[i].length, sgl_buf[i].flags);
            if (sgl_buf[i].flags & CHAIN_BIT) {
                sgl = sgl_buf[i].address;
                sgl_size = sgl_buf[i].length;
                break;
            }
            if (handle_bitflip_sge(pdev, sgl_buf[i])) {
                info_report("[TEST][ERROR] %s, handle "
                    "bitflip sge failed", __func__);
                return;
            }
        }
    } while (sgl != 0);

    pdev->irq_status.fields.bitflip_done = 1;
    msi_notify(&pdev->parent, 0);
}

static void init_bitflip(PciTestDev *pci_test_dev)
{
    pci_test_dev->bitflip_ctrl.value = 0;
    pci_test_dev->bitflip_sgl = 0;
    pci_test_dev->bitflip_data_buffer =
        aligned_alloc(64, PAGE_SIZE);
    if (pci_test_dev->bitflip_data_buffer == NULL) {
        info_report("[TEST][ERROR] %s, allocate "
            "internal buffer for bitwise not failed",
            __func__);
        return;
    }

    pci_test_dev->bitflip_sgl_buffer =
        aligned_alloc(64, PAGE_SIZE);
    if (pci_test_dev->bitflip_sgl_buffer == NULL) {
        info_report("[TEST][ERROR] %s, allocate addr list"
            "internal buffer for bitwise not failed",
            __func__);
        return;
    }
    memset(pci_test_dev->bitflip_data_buffer, 0,
        PAGE_SIZE);
    memset(pci_test_dev->bitflip_sgl_buffer, 0,
        PAGE_SIZE);

    timer_init_ms(&pci_test_dev->bitflip_timer,
        QEMU_CLOCK_VIRTUAL, bitflip_timer_callback,
        pci_test_dev);
}

static void reset_bitflip(PciTestDev *pci_test_dev)
{
    pci_test_dev->bitflip_ctrl.value = 0;
    pci_test_dev->bitflip_sgl = 0;
    memset(pci_test_dev->bitflip_data_buffer, 0,
        PAGE_SIZE);
    memset(pci_test_dev->bitflip_sgl_buffer, 0,
        PAGE_SIZE);
}

static void deinit_bitflip(PciTestDev *pci_test_dev)
{
    pci_test_dev->bitflip_ctrl.value = 0;
    pci_test_dev->bitflip_sgl = 0;

    if (pci_test_dev->bitflip_data_buffer) {
        free(pci_test_dev->bitflip_data_buffer);
        pci_test_dev->bitflip_data_buffer = NULL;
    }

    if (pci_test_dev->bitflip_sgl_buffer) {
        free(pci_test_dev->bitflip_sgl_buffer);
        pci_test_dev->bitflip_sgl_buffer = NULL;
    }

    timer_del(&pci_test_dev->bitflip_timer);
}

static uint64_t pci_test_dev_mmio_read(void *opaque, hwaddr addr,
    unsigned size)
{
    PciTestDev *pci_test_dev = opaque;
    uint64_t val = 0;

    switch (addr)
    {
    case REG_COUNTER:
        val = pci_test_dev->counter;
        break;
    case REG_IRQ_STATUS:
        val = pci_test_dev->irq_status.value;
        break;
    case REG_VECTOR_ADD_CTRL:
        val = pci_test_dev->vec_add_ctrl.value;
        break;
    case REG_BITFLIP_CTRL:
        val = pci_test_dev->bitflip_ctrl.value;
        break;
    default:
        memcpy(&val, pci_test_dev->mmio_regs + addr, size);
        break;
    }

    info_report("[TEST] %s, pci_test_dev=%p, rd mmio addr=%lx, "
        "val=%lx, size=%x", __func__, pci_test_dev, addr, val,
        size);

    return val;
}

static void pci_test_dev_mmio_write(void *opaque, hwaddr addr,
    uint64_t val, unsigned size)
{
    PciTestDev *pci_test_dev = opaque;

    info_report("[TEST] %s, pci_test_dev=%p, wr mmio addr=%lx, "
        "val=%lx, size=%x", __func__, pci_test_dev, addr, val,
        size);

    switch (addr)
    {
    case REG_ENABLE_TIMER:
        if (val != pci_test_dev->timer_enabled)
        {
            pci_test_dev->timer_enabled = val;
            pci_test_dev->counter = 0;
            if (pci_test_dev->timer_enabled)
            {
                timer_mod(&pci_test_dev->timer,
                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
            }
        }
        break;
    case REG_IRQ_STATUS:
        // write 1 to clear the bit
        pci_test_dev->irq_status.value &= ~val;
        break;
    case REG_VECTOR_ADD_CTRL:
        pci_test_dev->vec_add_ctrl.value = val;
        if (pci_test_dev->vec_add_ctrl.fields.doorbell) {
            info_report("[TEST] %s, vector add doorbell triggered",
                __func__);
            pci_test_dev->vec_add_ctrl.fields.doorbell = 0;
            timer_mod(&pci_test_dev->vector_add_timer,
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
        }
        break;
    case REG_VECTOR1_ADDR_LO:
    case REG_VECTOR2_ADDR_LO:
    case REG_VECTOR3_ADDR_LO:
    {
        uint32_t vecIdx = (addr - REG_VECTOR1_ADDR_LO) / 8;
        pci_test_dev->vector_addr[vecIdx] &= 0xFFFFFFFF00000000ULL;
        pci_test_dev->vector_addr[vecIdx] |= (val & 0xFFFFFFFFULL);
        memcpy(pci_test_dev->mmio_regs + addr, &val, size);
        break;
    }
    case REG_VECTOR1_ADDR_HI:
    case REG_VECTOR2_ADDR_HI:
    case REG_VECTOR3_ADDR_HI:
    {
        uint32_t vecIdx = (addr - REG_VECTOR1_ADDR_HI) / 8;
        pci_test_dev->vector_addr[vecIdx] &= 0x00000000FFFFFFFFULL;
        pci_test_dev->vector_addr[vecIdx] |= ((val & 0xFFFFFFFFULL) << 32);
        memcpy(pci_test_dev->mmio_regs + addr, &val, size);
        break;
    }
    case REG_BITFLIP_CTRL:
        pci_test_dev->bitflip_ctrl.value = val;
        if (pci_test_dev->bitflip_ctrl.fields.doorbell) {
            info_report("[TEST] %s, bitwise not doorbell triggered",
                __func__);
            pci_test_dev->bitflip_ctrl.fields.doorbell = 0;
            timer_mod(&pci_test_dev->bitflip_timer,
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
        }
        break;
    case REG_BITFLIP_SGL_LO:
        pci_test_dev->bitflip_sgl &= 0xFFFFFFFF00000000ULL;
        pci_test_dev->bitflip_sgl |= (val & 0xFFFFFFFFULL);
        memcpy(pci_test_dev->mmio_regs + addr, &val, size);
        break;
    case REG_BITFLIP_SGL_HI:
        pci_test_dev->bitflip_sgl &= 0x00000000FFFFFFFFULL;
        pci_test_dev->bitflip_sgl |= ((val & 0xFFFFFFFFULL) << 32);
        memcpy(pci_test_dev->mmio_regs + addr, &val, size);
        break;
    default:
        memcpy(pci_test_dev->mmio_regs + addr, &val, size);
        break;
    }
}

static const MemoryRegionOps pci_test_dev_mmio_ops = {
    .read = pci_test_dev_mmio_read,
    .write = pci_test_dev_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void pci_test_dev_write_config(PCIDevice *pci_dev,
    uint32_t address, uint32_t val, int len)
{
    info_report("[TEST] %s, address: 0x%x, val=0x%x",
        __func__, address, val);
    pci_default_write_config(pci_dev, address, val, len);
    pcie_cap_flr_write_config(pci_dev, address, val, len);
}

static void pci_test_dev_realize(PCIDevice *pci_dev, Error **errp)
{
    int ret = 0;
    PciTestDev *pci_test_dev = PCI_TEST_DEV(pci_dev);

    ret = msi_init(pci_dev, 0, 1, true, false, errp);
    if (ret < 0) {
        info_report("[TEST][ERROR] %s, msi_init failed, ret: %d",
            __func__, ret);
        goto done;
    }

    memory_region_init_io(&pci_test_dev->mmio, OBJECT(pci_test_dev),
        &pci_test_dev_mmio_ops, pci_test_dev, "pci-test-dev-mmio",
        MMIO_LENGTH);
    pci_test_dev->mmio_regs = malloc(MMIO_LENGTH);
    if (pci_test_dev->mmio_regs == NULL) {
        info_report("[TEST][ERROR] %s, allocate mmio regs failed",
            __func__);
        goto done;
    }

    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_MEM_TYPE_64,
        &pci_test_dev->mmio);

    ret = pci_pm_init(pci_dev, 0, errp);
    if (ret < 0) {
        info_report("[TEST][ERROR] %s, pci_pm_init failed, ret: %d",
            __func__, ret);
        goto done;
    }
    pci_set_word(pci_dev->config + ret + PCI_PM_PMC,
                 PCI_PM_CAP_VER_1_2);
    pci_set_word(pci_dev->wmask + ret + PCI_PM_CTRL,
                 PCI_PM_CTRL_STATE_MASK);

    ret = pcie_endpoint_cap_init(pci_dev, 0);
    if (ret < 0) {
        info_report("[TEST][ERROR] %s, pcie_endpoint_cap_init failed, "
            "ret: %d", __func__, ret);
        goto done;
    }

    pcie_cap_flr_init(pci_dev);
    pci_dev->config_write = pci_test_dev_write_config;

    info_report("[TEST] %s, vendor id: 0x%x, device id: 0x%x", __func__,
        PCI_DEVICE_GET_CLASS(pci_dev)->vendor_id,
        PCI_DEVICE_GET_CLASS(pci_dev)->device_id);

    create_timer(pci_test_dev);
    init_vector_add(pci_test_dev);
    init_bitflip(pci_test_dev);

done:
    if (ret < 0) {
        if (pci_test_dev->mmio_regs) {
            free(pci_test_dev->mmio_regs);
            pci_test_dev->mmio_regs = NULL;
        }
    }
}

static void pci_test_dev_exit(PCIDevice *pci_dev)
{
    PciTestDev *pci_test_dev = PCI_TEST_DEV(pci_dev);

    info_report("[TEST] %s, pci_test_dev=%p", __func__, pci_test_dev);

    if (pci_test_dev->mmio_regs) {
        free(pci_test_dev->mmio_regs);
        pci_test_dev->mmio_regs = NULL;
    }

    msi_uninit(pci_dev);
    destroy_timer(pci_test_dev);
    deinit_vector_add(pci_test_dev);
    deinit_bitflip(pci_test_dev);
}

static void pci_test_dev_reset(DeviceState *qdev)
{
    PCIDevice *pdev = PCI_DEVICE(qdev);
    PciTestDev *pci_test_dev = PCI_TEST_DEV(pdev);
    info_report("[TEST] %s, pci_test_dev=%p", __func__, pci_test_dev);
    memset(pci_test_dev->mmio_regs, 0, MMIO_LENGTH);
    reset_timer(pci_test_dev);
    reset_vector_add(pci_test_dev);
    reset_bitflip(pci_test_dev);
}

static void pci_test_dev_class_init(ObjectClass *oc,const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize = pci_test_dev_realize;
    pc->exit = pci_test_dev_exit;
    pc->vendor_id = PCI_VENDOR_ID_REDHAT;
    pc->device_id = PCI_DEVICE_ID_REDHAT_TEST;
    pc->class_id = PCI_CLASS_OTHERS;
    pc->revision = 1;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "PCI Test Device";
    device_class_set_legacy_reset(dc, pci_test_dev_reset);
}

static const TypeInfo test_dev_info = {
    .name          = TYPE_PCI_TEST_DEV,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PciTestDev),
    .class_init    = pci_test_dev_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void pci_test_dev_register_types(void)
{
    type_register_static(&test_dev_info);
}

type_init(pci_test_dev_register_types)
