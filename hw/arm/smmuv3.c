/*
 * Copyright (C) 2014-2016 Broadcom Corporation
 * Copyright (c) 2017 Red Hat, Inc.
 * Written by Prem Mallappa, Eric Auger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/boards.h"
#include "sysemu/sysemu.h"
#include "hw/sysbus.h"
#include "hw/pci/pci.h"
#include "exec/address-spaces.h"
#include "trace.h"
#include "qemu/error-report.h"

#include "hw/arm/smmuv3.h"
#include "smmuv3-internal.h"

static inline int smmu_enabled(SMMUV3State *s)
{
    return smmu_read32_reg(s, SMMU_REG_CR0) & SMMU_CR0_SMMU_ENABLE;
}

/**
 * smmu_irq_update - update the GERROR register according to
 * the IRQ and the enable state
 *
 * return > 0 when IRQ is supposed to be raised
 */
static int smmu_irq_update(SMMUV3State *s, int irq, uint64_t data)
{
    uint32_t error = 0;

    if (!smmu_gerror_irq_enabled(s)) {
        return 0;
    }

    switch (irq) {
    case SMMU_IRQ_EVTQ:
        if (smmu_evt_irq_enabled(s)) {
            error = SMMU_GERROR_EVENTQ;
        }
        break;
    case SMMU_IRQ_CMD_SYNC:
        if (smmu_gerror_irq_enabled(s)) {
            uint32_t err_type = (uint32_t)data;

            if (err_type) {
                uint32_t regval = smmu_read32_reg(s, SMMU_REG_CMDQ_CONS);
                smmu_write32_reg(s, SMMU_REG_CMDQ_CONS,
                                 regval | err_type << SMMU_CMD_CONS_ERR_SHIFT);
            }
            error = SMMU_GERROR_CMDQ;
        }
        break;
    case SMMU_IRQ_PRIQ:
        if (smmu_pri_irq_enabled(s)) {
            error = SMMU_GERROR_PRIQ;
        }
        break;
    }

    if (error) {
        uint32_t gerror = smmu_read32_reg(s, SMMU_REG_GERROR);
        uint32_t gerrorn = smmu_read32_reg(s, SMMU_REG_GERRORN);

        trace_smmuv3_irq_update(error, gerror, gerrorn);

        /* only toggle GERROR if the interrupt is not active */
        if (!((gerror ^ gerrorn) & error)) {
            smmu_write32_reg(s, SMMU_REG_GERROR, gerror ^ error);
        }
    }

    return error;
}

static void smmu_irq_raise(SMMUV3State *s, int irq, uint64_t data)
{
    trace_smmuv3_irq_raise(irq);
    if (smmu_irq_update(s, irq, data)) {
            qemu_irq_raise(s->irq[irq]);
    }
}

static MemTxResult smmu_q_read(SMMUV3State *s, SMMUQueue *q, void *data)
{
    uint64_t addr = Q_ENTRY(q, Q_IDX(q, q->cons));

    q->cons++;
    if (q->cons == q->entries) {
        q->cons = 0;
        q->wrap.cons++;     /* this will toggle */
    }

    return smmu_read_sysmem(addr, data, q->ent_size, false);
}

static MemTxResult smmu_q_write(SMMUV3State *s, SMMUQueue *q, void *data)
{
    uint64_t addr = Q_ENTRY(q, Q_IDX(q, q->prod));

    if (q->prod == q->entries) {
        q->prod = 0;
        q->wrap.prod++;     /* this will toggle */
    }

    q->prod++;

    smmu_write_sysmem(addr, data, q->ent_size, false);

    return MEMTX_OK;
}

static MemTxResult smmu_read_cmdq(SMMUV3State *s, Cmd *cmd)
{
    SMMUQueue *q = &s->cmdq;
    MemTxResult ret = smmu_q_read(s, q, cmd);
    uint32_t val = 0;

    val |= (q->wrap.cons << q->shift) | q->cons;

    /* Update consumer pointer */
    smmu_write32_reg(s, SMMU_REG_CMDQ_CONS, val);

    return ret;
}

static int smmu_cmdq_consume(SMMUV3State *s)
{
    uint32_t error = SMMU_CMD_ERR_NONE;

    trace_smmuv3_cmdq_consume(SMMU_CMDQ_ERR(s));

    if (!smmu_cmd_q_enabled(s)) {
        return 0;
    }

    while (!SMMU_CMDQ_ERR(s) && !smmu_is_q_empty(s, &s->cmdq)) {
        SMMUQueue *q = &s->cmdq;
        uint32_t type;
        Cmd cmd;

        if (smmu_read_cmdq(s, &cmd) != MEMTX_OK) {
            error = SMMU_CMD_ERR_ABORT;
            break;
        }

        trace_smmuv3_cmdq_consume_details(q->base, q->cons, q->prod,
                                          cmd.word[0], q->wrap.cons);

        type = CMD_TYPE(&cmd);

        switch (CMD_TYPE(&cmd)) {
        case SMMU_CMD_SYNC:     /* Fallthrough */
            if (CMD_CS(&cmd) & CMD_SYNC_SIG_IRQ) {
                smmu_irq_raise(s, SMMU_IRQ_CMD_SYNC, SMMU_CMD_ERR_NONE);
            } else if (CMD_CS(&cmd) & CMD_SYNC_SIG_SEV) {
                trace_smmuv3_cmdq_consume_sev();
            }
            break;
        case SMMU_CMD_PREFETCH_CONFIG:
        case SMMU_CMD_PREFETCH_ADDR:
        case SMMU_CMD_CFGI_STE:
        case SMMU_CMD_CFGI_STE_RANGE: /* same as SMMU_CMD_CFGI_ALL */
        case SMMU_CMD_CFGI_CD:
        case SMMU_CMD_CFGI_CD_ALL:
        case SMMU_CMD_TLBI_NH_ALL:
        case SMMU_CMD_TLBI_NH_ASID:
        case SMMU_CMD_TLBI_NH_VA:
        case SMMU_CMD_TLBI_NH_VAA:
        case SMMU_CMD_TLBI_EL3_ALL:
        case SMMU_CMD_TLBI_EL3_VA:
        case SMMU_CMD_TLBI_EL2_ALL:
        case SMMU_CMD_TLBI_EL2_ASID:
        case SMMU_CMD_TLBI_EL2_VA:
        case SMMU_CMD_TLBI_EL2_VAA:
        case SMMU_CMD_TLBI_S12_VMALL:
        case SMMU_CMD_TLBI_S2_IPA:
        case SMMU_CMD_TLBI_NSNH_ALL:
        case SMMU_CMD_ATC_INV:
        case SMMU_CMD_PRI_RESP:
        case SMMU_CMD_RESUME:
        case SMMU_CMD_STALL_TERM:
            trace_smmuv3_unhandled_cmd(type);
            break;
        default:
            error = SMMU_CMD_ERR_ILLEGAL;
            error_report("Illegal command type: %d, ignoring", CMD_TYPE(&cmd));
            dump_cmd(&cmd);
            break;
        }

        if (error != SMMU_CMD_ERR_NONE) {
            error_report("CMD Error");
            break;
        }
    }

    if (error) {
        smmu_irq_raise(s, SMMU_IRQ_GERROR, error);
    }

    trace_smmuv3_cmdq_consume_out(s->cmdq.wrap.prod, s->cmdq.prod,
                                  s->cmdq.wrap.cons, s->cmdq.cons);

    return 0;
}

/**
 * GERROR is updated when raising an interrupt, GERRORN will be updated
 * by SW and should match GERROR before normal operation resumes.
 */
static void smmu_irq_clear(SMMUV3State *s, uint64_t gerrorn)
{
    int irq = SMMU_IRQ_GERROR;
    uint32_t toggled;

    toggled = smmu_read32_reg(s, SMMU_REG_GERRORN) ^ gerrorn;

    while (toggled) {
        irq = ctz32(toggled);

        qemu_irq_lower(s->irq[irq]);

        toggled &= toggled - 1;
    }
}

static int smmu_evtq_update(SMMUV3State *s)
{
    if (!smmu_enabled(s)) {
        return 0;
    }

    if (!smmu_is_q_empty(s, &s->evtq)) {
        if (smmu_evt_irq_enabled(s)) {
            smmu_irq_raise(s, SMMU_IRQ_EVTQ, 0);
        }
    }

    if (smmu_is_q_empty(s, &s->evtq)) {
        smmu_irq_clear(s, SMMU_GERROR_EVENTQ);
    }

    return 1;
}

static void smmu_create_event(SMMUV3State *s, hwaddr iova,
                              uint32_t sid, bool is_write, int error);

static void smmu_update(SMMUV3State *s)
{
    int error = 0;

    /* SMMU starts processing commands even when not enabled */
    if (!smmu_enabled(s)) {
        goto check_cmdq;
    }

    /* EVENT Q updates takes more priority */
    if ((smmu_evt_q_enabled(s)) && !smmu_is_q_empty(s, &s->evtq)) {
        trace_smmuv3_update(smmu_is_q_empty(s, &s->evtq), s->evtq.prod,
                            s->evtq.cons, s->evtq.wrap.prod, s->evtq.wrap.cons);
        error = smmu_evtq_update(s);
    }

    if (error) {
        /* TODO: May be in future we create proper event queue entry */
        /* an error condition is not a recoverable event, like other devices */
        error_report("An unfavourable condition");
        smmu_create_event(s, 0, 0, 0, error);
    }

check_cmdq:
    if (smmu_cmd_q_enabled(s) && !SMMU_CMDQ_ERR(s)) {
        smmu_cmdq_consume(s);
    } else {
        trace_smmuv3_update_check_cmd(SMMU_CMDQ_ERR(s));
    }

}

static void smmu_update_irq(SMMUV3State *s, uint64_t addr, uint64_t val)
{
    smmu_irq_clear(s, val);

    smmu_write32_reg(s, SMMU_REG_GERRORN, val);

    trace_smmuv3_update_irq(smmu_is_irq_pending(s, 0),
                          smmu_read32_reg(s, SMMU_REG_GERROR),
                          smmu_read32_reg(s, SMMU_REG_GERRORN));

    /* Clear only when no more left */
    if (!smmu_is_irq_pending(s, 0)) {
        qemu_irq_lower(s->irq[0]);
    }
}

#define SMMU_ID_REG_INIT(s, reg, d) do {        \
    s->regs[reg >> 2] = d;                      \
    } while (0)

static void smmuv3_id_reg_init(SMMUV3State *s)
{
    uint32_t data =
        SMMU_IDR0_STLEVEL << SMMU_IDR0_STLEVEL_SHIFT |
        SMMU_IDR0_TERM    << SMMU_IDR0_TERM_SHIFT    |
        SMMU_IDR0_STALL   << SMMU_IDR0_STALL_SHIFT   |
        SMMU_IDR0_VMID16  << SMMU_IDR0_VMID16_SHIFT  |
        SMMU_IDR0_PRI     << SMMU_IDR0_PRI_SHIFT     |
        SMMU_IDR0_ASID16  << SMMU_IDR0_ASID16_SHIFT  |
        SMMU_IDR0_ATS     << SMMU_IDR0_ATS_SHIFT     |
        SMMU_IDR0_HYP     << SMMU_IDR0_HYP_SHIFT     |
        SMMU_IDR0_HTTU    << SMMU_IDR0_HTTU_SHIFT    |
        SMMU_IDR0_COHACC  << SMMU_IDR0_COHACC_SHIFT  |
        SMMU_IDR0_TTF     << SMMU_IDR0_TTF_SHIFT     |
        SMMU_IDR0_S1P     << SMMU_IDR0_S1P_SHIFT     |
        SMMU_IDR0_S2P     << SMMU_IDR0_S2P_SHIFT;

    SMMU_ID_REG_INIT(s, SMMU_REG_IDR0, data);

#define SMMU_QUEUE_SIZE_LOG2  19
    data =
        1 << 27 |                    /* Attr Types override */
        SMMU_QUEUE_SIZE_LOG2 << 21 | /* Cmd Q size */
        SMMU_QUEUE_SIZE_LOG2 << 16 | /* Event Q size */
        SMMU_QUEUE_SIZE_LOG2 << 11 | /* PRI Q size */
        0  << 6 |                    /* SSID not supported */
        SMMU_IDR1_SIDSIZE;

    SMMU_ID_REG_INIT(s, SMMU_REG_IDR1, data);

    data =
        SMMU_IDR5_GRAN << SMMU_IDR5_GRAN_SHIFT | SMMU_IDR5_OAS;

    SMMU_ID_REG_INIT(s, SMMU_REG_IDR5, data);

}

static void smmuv3_init(SMMUV3State *s)
{
    smmuv3_id_reg_init(s);      /* Update ID regs alone */

    s->sid_size = SMMU_IDR1_SIDSIZE;

    s->cmdq.entries = (smmu_read32_reg(s, SMMU_REG_IDR1) >> 21) & 0x1f;
    s->cmdq.ent_size = sizeof(Cmd);
    s->evtq.entries = (smmu_read32_reg(s, SMMU_REG_IDR1) >> 16) & 0x1f;
    s->evtq.ent_size = sizeof(Evt);
}

/*
 * All SMMU data structures are little endian, and are aligned to 8 bytes
 * L1STE/STE/L1CD/CD, Queue entries in CMDQ/EVTQ/PRIQ
 */
static inline int smmu_get_ste(SMMUV3State *s, hwaddr addr, Ste *buf)
{
    int ret;

    trace_smmuv3_get_ste(addr);
    ret = dma_memory_read(&address_space_memory, addr, buf, sizeof(*buf));
    dump_ste(buf);
    return ret;
}

/*
 * For now we only support CD with a single entry, 'ssid' is used to identify
 * otherwise
 */
static inline int smmu_get_cd(SMMUV3State *s, Ste *ste, uint32_t ssid, Cd *buf)
{
    hwaddr addr = STE_CTXPTR(ste);
    int ret;

    if (STE_S1CDMAX(ste) != 0) {
        error_report("Multilevel Ctx Descriptor not supported yet");
    }

    ret = dma_memory_read(&address_space_memory, addr, buf, sizeof(*buf));

    trace_smmuv3_get_cd(addr);
    dump_cd(cd);

    return ret;
}

/**
 * is_ste_consistent - Check validity of STE
 * according to 6.2.1 Validty of STE
 * TODO: check the relevance of each check and compliance
 * with this spec chapter
 */
static int is_ste_consistent(SMMUV3State *s, Ste *ste)
{
    uint32_t _config = STE_CONFIG(ste);
    uint32_t ste_vmid, ste_eats, ste_s2s, ste_s1fmt, ste_s2aa64, ste_s1cdmax;
    uint32_t ste_strw;
    bool strw_unused, addr_out_of_range, granule_supported;
    bool config[] = {_config & 0x1, _config & 0x2, _config & 0x3};

    ste_vmid = STE_S2VMID(ste);
    ste_eats = STE_EATS(ste); /* Enable PCIe ATS trans */
    ste_s2s = STE_S2S(ste);
    ste_s1fmt = STE_S1FMT(ste);
    ste_s2aa64 = STE_S2AA64(ste);
    ste_s1cdmax = STE_S1CDMAX(ste); /*CD bit # S1ContextPtr */
    ste_strw = STE_STRW(ste); /* stream world control */

    if (!STE_VALID(ste)) {
        error_report("STE NOT valid");
        return false;
    }

    granule_supported = is_s2granule_valid(ste);

    /* As S1/S2 combinations are supported do not check
     * corresponding STE config values */

    if (!config[2]) {
        /* Report abort to device, no event recorded */
        error_report("STE config 0b000 not implemented");
        return false;
    }

    if (!SMMU_IDR1_SIDSIZE && ste_s1cdmax && config[0] &&
        !SMMU_IDR0_CD2L && (ste_s1fmt == 1 || ste_s1fmt == 2)) {
        error_report("STE inconsistant, CD mismatch");
        return false;
    }
    if (SMMU_IDR0_ATS && ((_config & 0x3) == 0) &&
        ((ste_eats == 2 && (_config != 0x7 || ste_s2s)) ||
        (ste_eats == 1 && !ste_s2s))) {
        error_report("STE inconsistant, EATS/S2S mismatch");
        return false;
    }
    if (config[0] && (SMMU_IDR1_SIDSIZE &&
        (ste_s1cdmax > SMMU_IDR1_SIDSIZE))) {
        error_report("STE inconsistant, SSID out of range");
        return false;
    }

    strw_unused = (!SMMU_IDR0_S1P || !SMMU_IDR0_HYP || (_config == 4));

    addr_out_of_range = STE_S2TTB(ste) > MAX_PA(ste);

    if (has_stage2(ste)) {
        if ((ste_s2aa64 && !is_s2granule_valid(ste)) ||
            (!ste_s2aa64 && !(SMMU_IDR0_TTF & 0x1)) ||
            (ste_s2aa64 && !(SMMU_IDR0_TTF & 0x2))  ||
            ((STE_S2HA(ste) || STE_S2HD(ste)) && !ste_s2aa64) ||
            ((STE_S2HA(ste) || STE_S2HD(ste)) && !SMMU_IDR0_HTTU) ||
            (STE_S2HD(ste) && (SMMU_IDR0_HTTU == 1)) || addr_out_of_range) {
            error_report("STE inconsistant");
            trace_smmuv3_is_ste_consistent(config[1], granule_supported,
                                           addr_out_of_range, ste_s2aa64,
                                           STE_S2HA(ste), STE_S2HD(ste),
                                           STE_S2TTB(ste));
            return false;
        }
    }
    if (SMMU_IDR0_S2P && (config[0] == 0 && config[1]) &&
        (strw_unused || !ste_strw) && !SMMU_IDR0_VMID16 && !(ste_vmid >> 8)) {
        error_report("STE inconsistant, VMID out of range");
        return false;
    }

    return true;
}

/**
 * smmu_find_ste - Return the stream table entry associated
 * to the sid
 *
 * @s: smmuv3 handle
 * @sid: stream ID
 * @ste: returned stream table entry
 * Supports linear and 2-level stream table
 */
static int smmu_find_ste(SMMUV3State *s, uint16_t sid, Ste *ste)
{
    hwaddr addr;

    trace_smmuv3_find_ste(sid, s->features, s->sid_split);
    /* Check SID range */
    if (sid > (1 << s->sid_size)) {
        return SMMU_EVT_C_BAD_SID;
    }
    if (s->features & SMMU_FEATURE_2LVL_STE) {
        int l1_ste_offset, l2_ste_offset, max_l2_ste, span;
        hwaddr l1ptr, l2ptr;
        STEDesc l1std;

        l1_ste_offset = sid >> s->sid_split;
        l2_ste_offset = sid & ((1 << s->sid_split) - 1);
        l1ptr = (hwaddr)(s->strtab_base + l1_ste_offset * sizeof(l1std));
        smmu_read_sysmem(l1ptr, &l1std, sizeof(l1std), false);
        span = L1STD_SPAN(&l1std);

        if (!span) {
            /* l2ptr is not valid */
            error_report("invalid sid=%d (L1STD span=0)", sid);
            return SMMU_EVT_C_BAD_SID;
        }
        max_l2_ste = (1 << span) - 1;
        l2ptr = L1STD_L2PTR(&l1std);
        trace_smmuv3_find_ste_2lvl(s->strtab_base, l1ptr, l1_ste_offset,
                                   l2ptr, l2_ste_offset, max_l2_ste);
        if (l2_ste_offset > max_l2_ste) {
            error_report("l2_ste_offset=%d > max_l2_ste=%d",
                         l2_ste_offset, max_l2_ste);
            return SMMU_EVT_C_BAD_STE;
        }
        addr = L1STD_L2PTR(&l1std) + l2_ste_offset * sizeof(*ste);
    } else {
        addr = s->strtab_base + sid * sizeof(*ste);
    }

    if (smmu_get_ste(s, addr, ste)) {
        error_report("Unable to Fetch STE");
        return SMMU_EVT_F_UUT;
    }

    return 0;
}

/**
 * smmu_cfg_populate_s1 - Populate the stage 1 translation config
 * from the context descriptor
 */
static int smmu_cfg_populate_s1(SMMUTransCfg *cfg, Cd *cd)
{
    bool s1a64 = CD_AARCH64(cd);
    int epd0 = CD_EPD0(cd);
    int tg;

    cfg->stage   = 1;
    tg           = epd0 ? CD_TG1(cd) : CD_TG0(cd);
    cfg->tsz     = epd0 ? CD_T1SZ(cd) : CD_T0SZ(cd);
    cfg->ttbr    = epd0 ? CD_TTB1(cd) : CD_TTB0(cd);
    cfg->oas     = oas2bits(CD_IPS(cd));

    if (s1a64) {
        cfg->tsz = MIN(cfg->tsz, 39);
        cfg->tsz = MAX(cfg->tsz, 16);
    }
    cfg->granule_sz = tg2granule(tg, epd0);

    cfg->oas = MIN(oas2bits(SMMU_IDR5_OAS), cfg->oas);
    /* fix ttbr - make top bits zero*/
    cfg->ttbr = extract64(cfg->ttbr, 0, cfg->oas);
    cfg->aa64 = s1a64;

    trace_smmuv3_cfg_stage(cfg->stage, cfg->oas, cfg->tsz, cfg->ttbr,
                           cfg->aa64, cfg->granule_sz);

    return 0;
}

/**
 * smmu_cfg_populate_s2 - Populate the stage 2 translation config
 * from the Stream Table Entry
 */
static int smmu_cfg_populate_s2(SMMUTransCfg *cfg, Ste *ste)
{
    bool s2a64 = STE_S2AA64(ste);
    int tg;

    if (cfg->stage) {
        error_report("%s nested S1 + S2 is not supported", __func__);
    } else {
        /* S2 only */
        cfg->stage = 2;
    }

    tg           = STE_S2TG(ste);
    cfg->tsz     = STE_S2T0SZ(ste);
    cfg->ttbr    = STE_S2TTB(ste);
    cfg->oas     = pa_range(ste);

    cfg->aa64    = s2a64;

    if (s2a64) {
        cfg->tsz = MIN(cfg->tsz, 39);
        cfg->tsz = MAX(cfg->tsz, 16);
    }
    cfg->granule_sz = tg2granule(tg, 0);

    cfg->oas = MIN(oas2bits(SMMU_IDR5_OAS), cfg->oas);
    /* fix ttbr - make top bits zero*/
    cfg->ttbr = extract64(cfg->ttbr, 0, cfg->oas);

    trace_smmuv3_cfg_stage(cfg->stage, cfg->oas, cfg->tsz, cfg->ttbr,
                           cfg->aa64, cfg->granule_sz);

    return 0;
}

/**
 * smmu_cfg_populate - Populates the translation config corresponding
 * to the STE and CD content
 */
static int smmu_cfg_populate(Ste *ste, Cd *cd, SMMUTransCfg *cfg)
{
    int ret = 0;

    if (is_ste_bypass(ste)) {
        return ret;
    }

    if (has_stage1(ste)) {
        ret = smmu_cfg_populate_s1(cfg, cd);
        if (ret) {
            return ret;
        }
    }
    if (has_stage2(ste)) {
        ret = smmu_cfg_populate_s2(cfg, ste);
        if (ret) {
            return ret;
        }
    }
    return ret;
}

static SMMUEvtErr smmu_walk_pgtable(SMMUV3State *s, SMMUTransCfg *cfg,
                                    IOMMUTLBEntry *tlbe, bool is_write)
{
    SMMUState *sys = SMMU_SYS_DEV(s);
    SMMUBaseClass *sbc = SMMU_DEVICE_GET_CLASS(sys);
    SMMUEvtErr error = 0;
    uint32_t page_size = 0, perm = 0;

    trace_smmuv3_walk_pgtable(tlbe->iova, is_write);

    if (!cfg->stage) {
        return 0;
    }

    cfg->input = tlbe->iova;

    if (cfg->aa64) {
        error = sbc->translate_64(cfg, &page_size, &perm, is_write);
    } else {
        error = sbc->translate_32(cfg, &page_size, &perm, is_write);
    }

    if (error) {
        error_report("PTW failed for iova=0x%"PRIx64" is_write=%d (%d)",
                     cfg->input, is_write, error);
        goto exit;
    }
    tlbe->translated_addr = cfg->output;
    tlbe->addr_mask = page_size - 1;
    tlbe->perm = perm;

    trace_smmuv3_walk_pgtable_out(tlbe->translated_addr,
                                  tlbe->addr_mask, tlbe->perm);
exit:
    return error;
}

static MemTxResult smmu_write_evtq(SMMUV3State *s, Evt *evt)
{
    SMMUQueue *q = &s->evtq;
    int ret = smmu_q_write(s, q, evt);
    uint32_t val = 0;

    val |= (q->wrap.prod << q->shift) | q->prod;

    smmu_write32_reg(s, SMMU_REG_EVTQ_PROD, val);

    return ret;
}

/*
 * Events created on the EventQ
 */
static void smmu_create_event(SMMUV3State *s, hwaddr iova,
                              uint32_t sid, bool is_write, int error)
{
    SMMUQueue *q = &s->evtq;
    uint64_t head;
    Evt evt;

    if (!smmu_evt_q_enabled(s)) {
        return;
    }

    EVT_SET_TYPE(&evt, error);
    EVT_SET_SID(&evt, sid);

    switch (error) {
    case SMMU_EVT_F_UUT:
    case SMMU_EVT_C_BAD_STE:
        break;
    case SMMU_EVT_C_BAD_CD:
    case SMMU_EVT_F_CD_FETCH:
        break;
    case SMMU_EVT_F_TRANS_FORBIDDEN:
    case SMMU_EVT_F_WALK_EXT_ABRT:
        EVT_SET_INPUT_ADDR(&evt, iova);
    default:
        break;
    }

    smmu_write_evtq(s, &evt);

    head = Q_IDX(q, q->prod);

    if (smmu_is_q_full(s, &s->evtq)) {
        head = q->prod ^ (1 << 31);     /* Set overflow */
    }

    smmu_write32_reg(s, SMMU_REG_EVTQ_PROD, head);

    smmu_irq_raise(s, SMMU_IRQ_EVTQ, 0);
}

/*
 * TR - Translation Request
 * TT - Translated Tansaction
 * OT - Other Transaction
 */
static IOMMUTLBEntry
smmuv3_translate(MemoryRegion *mr, hwaddr addr, bool is_write)
{
    SMMUDevice *sdev = container_of(mr, SMMUDevice, iommu);
    SMMUV3State *s = sdev->smmu;
    SMMUTransCfg transcfg = {};
    uint16_t sid = 0;
    Ste ste;
    Cd cd;
    SMMUEvtErr error = 0;

    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova = addr,
        .translated_addr = addr,
        .addr_mask = ~(hwaddr)0,
        .perm = IOMMU_NONE,
    };

    sid = smmu_get_sid(sdev);

    /* SMMU Bypass, We allow traffic through if SMMU is disabled  */
    if (!smmu_enabled(s)) {
        trace_smmuv3_translate_bypass(mr->name, sid, addr, is_write);
        goto out;
    }

    trace_smmuv3_translate_in(sid, pci_bus_num(sdev->bus), s->strtab_base);

    /* Fetch & Check STE */
    error = smmu_find_ste(s, sid, &ste);
    if (error) {
        goto error_out;  /* F_STE_FETCH or F_CFG_CONFLICT */
    }

    if (STE_VALID(&ste) && is_ste_bypass(&ste)) {
        trace_smmuv3_translate_bypass(mr->name, sid, addr, is_write);
        goto out;
    }

    if (!is_ste_consistent(s, &ste)) {
        error = SMMU_EVT_C_BAD_STE;
        goto error_out;
    }

    if (has_stage1(&ste)) { /* Stage 1 */
        smmu_get_cd(s, &ste, 0, &cd); /* We dont have SSID yet, so 0 */

        if (!is_cd_valid(s, &ste, &cd)) {
            error = SMMU_EVT_C_BAD_CD;
            goto error_out;
        }
    }

    smmu_cfg_populate(&ste, &cd, &transcfg);

    /* Walk Stage1, if S2 is enabled, S2 walked for Every access on S1 */
    error = smmu_walk_pgtable(s, &transcfg, &entry, is_write);

    entry.perm = is_write ? IOMMU_RW : IOMMU_RO;

    trace_smmuv3_translate_ok(mr->name, sid, addr,
                              entry.translated_addr, entry.perm);

error_out:
    if (error > 1) {
        error_report("Translation Error: %x", error);
        smmu_create_event(s, entry.iova, sid, is_write, error);
    }

out:
    return entry;
}

static inline void smmu_update_base_reg(SMMUV3State *s, uint64_t *base,
                                        uint64_t val)
{
    *base = val & ~(SMMU_BASE_RA | 0x3fULL);
}

static void smmu_update_qreg(SMMUV3State *s, SMMUQueue *q, hwaddr reg,
                             uint32_t off, uint64_t val, unsigned size)
{
    if (size == 8 && off == 0) {
        smmu_write64_reg(s, reg, val);
    } else {
        smmu_write_reg(s, reg, val);
    }

    switch (off) {
    case 0:                             /* BASE register */
        val = smmu_read64_reg(s, reg);
        q->shift = val & 0x1f;
        q->entries = 1 << (q->shift);
        smmu_update_base_reg(s, &q->base, val);
        break;

    case 4:                             /* CONS */
        q->cons = Q_IDX(q, val);
        q->wrap.cons = val >> q->shift;
        trace_smmuv3_update_qreg(q->cons, val);
        break;

    case 8:                             /* PROD */
        q->prod = Q_IDX(q, val);
        q->wrap.prod = val >> q->shift;
        break;
    }

    switch (reg) {
    case SMMU_REG_CMDQ_PROD:            /* should be only for CMDQ_PROD */
    case SMMU_REG_CMDQ_CONS:            /* but we do it anyway */
        smmu_update(s);
        break;
    }
}

static void smmu_write_mmio_fixup(SMMUV3State *s, hwaddr *addr)
{
    switch (*addr) {
    case 0x100a8: case 0x100ac:         /* Aliasing => page0 registers */
    case 0x100c8: case 0x100cc:
        *addr ^= (hwaddr)0x10000;
    }
}

static void smmu_write_mmio(void *opaque, hwaddr addr,
                            uint64_t val, unsigned size)
{
    SMMUState *sys = opaque;
    SMMUV3State *s = SMMU_V3_DEV(sys);
    bool update = false;

    smmu_write_mmio_fixup(s, &addr);

    trace_smmuv3_write_mmio(addr, val);

    switch (addr) {
    case 0xFDC ... 0xFFC:
    case SMMU_REG_IDR0 ... SMMU_REG_IDR5:
        trace_smmuv3_write_mmio_idr(addr, val);
        return;

    case SMMU_REG_GERRORN:
        smmu_update_irq(s, addr, val);
        return;

    case SMMU_REG_CR0:
        smmu_write32_reg(s, SMMU_REG_CR0, val);
        smmu_write32_reg(s, SMMU_REG_CR0_ACK, val);
        update = true;
        break;

    case SMMU_REG_IRQ_CTRL:
        smmu_write32_reg(s, SMMU_REG_IRQ_CTRL_ACK, val);
        update = true;
        break;

    case SMMU_REG_STRTAB_BASE:
        smmu_update_base_reg(s, &s->strtab_base, val);
        return;

    case SMMU_REG_STRTAB_BASE_CFG:
        if (((val >> 16) & 0x3) == 0x1) {
            s->sid_split = (val >> 6) & 0x1f;
            s->features |= SMMU_FEATURE_2LVL_STE;
        }
        break;

    case SMMU_REG_CMDQ_PROD:
    case SMMU_REG_CMDQ_CONS:
    case SMMU_REG_CMDQ_BASE:
    case SMMU_REG_CMDQ_BASE + 4:
        smmu_update_qreg(s, &s->cmdq, addr, addr - SMMU_REG_CMDQ_BASE,
                         val, size);
        return;

    case SMMU_REG_EVTQ_CONS:            /* fallthrough */
    {
        SMMUQueue *evtq = &s->evtq;
        evtq->cons = Q_IDX(evtq, val);
        evtq->wrap.cons = Q_WRAP(evtq, val);

        trace_smmuv3_write_mmio_evtq_cons_bef_clear(evtq->prod, evtq->cons,
                                                    evtq->wrap.prod,
                                                    evtq->wrap.cons);
        if (smmu_is_q_empty(s, &s->evtq)) {
            trace_smmuv3_write_mmio_evtq_cons_after_clear(evtq->prod,
                                                          evtq->cons,
                                                          evtq->wrap.prod,
                                                          evtq->wrap.cons);
            qemu_irq_lower(s->irq[SMMU_IRQ_EVTQ]);
        }
    }
    case SMMU_REG_EVTQ_BASE:
    case SMMU_REG_EVTQ_BASE + 4:
    case SMMU_REG_EVTQ_PROD:
        smmu_update_qreg(s, &s->evtq, addr, addr - SMMU_REG_EVTQ_BASE,
                         val, size);
        return;

    case SMMU_REG_PRIQ_CONS:
    case SMMU_REG_PRIQ_BASE:
    case SMMU_REG_PRIQ_BASE + 4:
    case SMMU_REG_PRIQ_PROD:
        smmu_update_qreg(s, &s->priq, addr, addr - SMMU_REG_PRIQ_BASE,
                         val, size);
        return;
    }

    if (size == 8) {
        smmu_write_reg(s, addr, val);
    } else {
        smmu_write32_reg(s, addr, (uint32_t)val);
    }

    if (update) {
        smmu_update(s);
    }
}

static uint64_t smmu_read_mmio(void *opaque, hwaddr addr, unsigned size)
{
    SMMUState *sys = opaque;
    SMMUV3State *s = SMMU_V3_DEV(sys);
    uint64_t val;

    smmu_write_mmio_fixup(s, &addr);

    /* Primecell/Corelink ID registers */
    switch (addr) {
    case 0xFF0 ... 0xFFC:
    case 0xFDC ... 0xFE4:
        val = 0;
        error_report("addr:0x%"PRIx64" val:0x%"PRIx64, addr, val);
        break;

    default:
        val = (uint64_t)smmu_read32_reg(s, addr);
        break;

    case SMMU_REG_STRTAB_BASE ... SMMU_REG_CMDQ_BASE:
    case SMMU_REG_EVTQ_BASE:
    case SMMU_REG_PRIQ_BASE ... SMMU_REG_PRIQ_IRQ_CFG1:
        val = smmu_read64_reg(s, addr);
        break;
    }

    trace_smmuv3_read_mmio(addr, val, s->cmdq.cons);
    return val;
}

static const MemoryRegionOps smmu_mem_ops = {
    .read = smmu_read_mmio,
    .write = smmu_write_mmio,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void smmu_init_irq(SMMUV3State *s, SysBusDevice *dev)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(s->irq); i++) {
        sysbus_init_irq(dev, &s->irq[i]);
    }
}

static AddressSpace *smmu_find_add_as(PCIBus *bus, void *opaque, int devfn)
{
    SMMUState *s = opaque;
    uintptr_t key = (uintptr_t)bus;
    SMMUPciBus *sbus = g_hash_table_lookup(s->smmu_as_by_busptr, &key);
    SMMUDevice *sdev;

    if (!sbus) {
        uintptr_t *new_key = g_malloc(sizeof(*new_key));

        *new_key = (uintptr_t)bus;
        sbus = g_malloc0(sizeof(SMMUPciBus) +
                         sizeof(SMMUDevice *) * SMMU_PCI_DEVFN_MAX);
        sbus->bus = bus;
        g_hash_table_insert(s->smmu_as_by_busptr, new_key, sbus);
    }

    sdev = sbus->pbdev[devfn];
    if (!sdev) {
        sdev = sbus->pbdev[devfn] = g_malloc0(sizeof(SMMUDevice));

        sdev->smmu = s;
        sdev->bus = bus;
        sdev->devfn = devfn;

        memory_region_init_iommu(&sdev->iommu, OBJECT(s),
                                 &s->iommu_ops, TYPE_SMMU_V3_DEV,
                                 UINT64_MAX);
        address_space_init(&sdev->as, &sdev->iommu, TYPE_SMMU_V3_DEV);
    }

    return &sdev->as;

}

static void smmu_init_iommu_as(SMMUV3State *sys)
{
    SMMUState *s = SMMU_SYS_DEV(sys);
    PCIBus *pcibus = pci_find_primary_bus();

    if (pcibus) {
        pci_setup_iommu(pcibus, smmu_find_add_as, s);
    } else {
        error_report("No PCI bus, SMMU is not registered");
    }
}

static void smmu_reset(DeviceState *dev)
{
    SMMUV3State *s = SMMU_V3_DEV(dev);
    smmuv3_init(s);
}

static int smmu_populate_internal_state(void *opaque, int version_id)
{
    SMMUV3State *s = opaque;

    smmu_update(s);
    return 0;
}

static void smmu_realize(DeviceState *d, Error **errp)
{
    SMMUState *sys = SMMU_SYS_DEV(d);
    SMMUV3State *s = SMMU_V3_DEV(sys);
    SysBusDevice *dev = SYS_BUS_DEVICE(d);

    sys->iommu_ops.translate = smmuv3_translate;
    sys->iommu_ops.notify_flag_changed = NULL;
    /* Register Access */
    memset(sys->smmu_as_by_bus_num, 0, sizeof(sys->smmu_as_by_bus_num));
    memory_region_init_io(&sys->iomem, OBJECT(s),
                          &smmu_mem_ops, sys, TYPE_SMMU_V3_DEV, 0x20000);

    sys->smmu_as_by_busptr = g_hash_table_new_full(smmu_uint64_hash,
                                                   smmu_uint64_equal,
                                                   g_free, g_free);
    sysbus_init_mmio(dev, &sys->iomem);

    smmu_init_irq(s, dev);

    smmu_init_iommu_as(s);
}

static const VMStateDescription vmstate_smmuv3 = {
    .name = "smmuv3",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = smmu_populate_internal_state,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64_ARRAY(regs, SMMUV3State, SMMU_NREGS),
        VMSTATE_END_OF_LIST(),
    },
};

static void smmuv3_instance_init(Object *obj)
{
    /* Nothing much to do here as of now */
}

static void smmuv3_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset   = smmu_reset;
    dc->vmsd    = &vmstate_smmuv3;
    dc->realize = smmu_realize;
}

static const TypeInfo smmuv3_type_info = {
    .name          = TYPE_SMMU_V3_DEV,
    .parent        = TYPE_SMMU_DEV_BASE,
    .instance_size = sizeof(SMMUV3State),
    .instance_init = smmuv3_instance_init,
    .class_data    = NULL,
    .class_size    = sizeof(SMMUV3Class),
    .class_init    = smmuv3_class_init,
};

static void smmuv3_register_types(void)
{
    type_register(&smmuv3_type_info);
}

type_init(smmuv3_register_types)

