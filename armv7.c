#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "darm.h"
#include "armv7-tbl.h"

#define BITMSK_12 ((1 << 12) - 1)
#define BITMSK_24 ((1 << 24) - 1)

struct {
    const char *mnemonic_extension;
    const char *meaning_integer;
    const char *meaning_fp;
} g_condition_codes[] = {
    {"EQ", "Equal", "Equal"},
    {"NE", "Not equal", "Not equal, or unordered"},
    {"CS", "Carry Set", "Greater than, equal, or unordered"},
    {"CC", "Carry Clear", "Less than"},
    {"MI", "Minus, negative", "Less than"},
    {"PL", "Plus, positive or zero", "Greater than, equal, or unordered"},
    {"VS", "Overflow", "Unordered"},
    {"VC", "No overflow", "Not unordered"},
    {"HI", "Unsigned higher", "Greater than, unordered"},
    {"LS", "Unsigned lower or same", "Greater than, or unordered"},
    {"GE", "Signed greater than or equal", "Greater than, or unordered"},
    {"LT", "Signed less than", "Less than, or unordered"},
    {"GT", "Signed greater than", "Greater than"},
    {"LE", "Signed less than or equal", "Less than, equal, or unordered"},
    {"AL", "Always (unconditional)", "Always (unconditional)"},

    // alias for CS
    {"HS", "Carry Set", "Greater than, equal, or unordered"},
    // alias for CC
    {"LO", "Carry Clear", "Less than"},
};

const char *armv7_condition_info(int condition_flag,
    const char **meaning_integer, const char **meaning_fp,
    int omit_always_mnemonic)
{
    if(condition_flag < 0 || condition_flag > 0b1110) return NULL;

    if(meaning_integer != NULL) {
        *meaning_integer = g_condition_codes[condition_flag].meaning_integer;
    }

    if(meaning_fp != NULL) {
        *meaning_fp = g_condition_codes[condition_flag].meaning_fp;
    }

    // the "AL" mnemonic extension can be omitted
    if(omit_always_mnemonic != 0 && condition_flag == 0b1110) {
        return "";
    }

    // return the mnemonic extension
    return g_condition_codes[condition_flag].mnemonic_extension;
}

int armv7_condition_index(const char *condition_code)
{
    if(condition_code == NULL) return -1;

    // the "AL" condition flag
    if(condition_code[0] == 0) return 0b1110;

    for (uint32_t i = 0; i < ARRAYSIZE(g_condition_codes); i++) {
        if(!strcmp(condition_code, g_condition_codes[i].mnemonic_extension)) {
            return i;
        }
    }

    return -1;
}

static const char *shift_types[] = {
    "LSL", "LSR", "ASR", "ROR",
};

void armv7_shift_decode(darm_t *d, const char **type, uint32_t *immediate)
{
    if(d->type == 0 && d->Rs == 0) {
        *type = NULL, *immediate = 0;
    }
    else if(d->type == 0b11 && d->Rs == 0) {
        *type = "RRX", *immediate = 0;
    }
    else {
        *type = shift_types[d->type];
        *immediate = d->Rs;

        // 32 is encoded as 0
        if((d->type == 0b01 || d->type == 0b10) && d->Rs == 0) {
            *immediate = 32;
        }
    }
}

static int armv7_disas_cond(darm_t *d, uint32_t w)
{
    // the instruction label
    d->instr = armv7_instr_labels[(w >> 20) & 0xff];
    d->instr_type = armv7_instr_types[(w >> 20) & 0xff];

    // do a lookup for the type of instruction
    switch (d->instr_type) {
    case T_INVLD:
        return -1;

    case T_ARITH_SHIFT:
        d->S = (w >> 20) & 1;
        d->Rd = (w >> 12) & 0b1111;
        d->Rn = (w >> 16) & 0b1111;
        d->Rm = w & 0b1111;
        d->type = (w >> 5) & 0b11;

        // type == 1, shift with the value of the lower bits of Rs
        d->shift_is_reg = (w >> 4) & 1;
        if(d->shift_is_reg != 0) {
            d->Rs = (w >> 8) & 0b1111;
        }
        else {
            d->shift = (w >> 7) & 0b11111;
        }
        return 0;

    case T_ARITH_IMM:
        d->S = (w >> 20) & 1;
        d->Rd = (w >> 12) & 0b1111;
        d->Rn = (w >> 16) & 0b1111;
        d->imm = w & BITMSK_12;

        // check whether this instruction is in fact an ADR instruction
        if((d->instr == I_ADD || d->instr == I_SUB) &&
                d->S == 0 && d->Rn == PC) {
            d->instr = I_ADR, d->Rn = 0;
            d->add = (w >> 23) & 1;
        }
        return 0;

    case T_BRNCHSC:
        d->imm = w & BITMSK_24;

        // if the instruction is B or BL, then we have to sign-extend it and
        // multiply it with four
        if(d->instr != I_SVC) {
            // check if the highest bit of the imm24 is set, if so, we
            // manually sign-extend the integer
            if((d->imm >> 23) & 1) {
                d->imm = (d->imm | 0xff000000) << 2;
            }
            else {
                d->imm = d->imm << 2;
            }
        }
        return 0;

    case T_BRNCHMISC:
        // first get the real instruction label
        d->instr = type4_instr_lookup[(w >> 4) & 0b1111];

        // now we do a switch statement based on the instruction label,
        // rather than some magic values
        switch ((uint32_t) d->instr) {
        case I_BKPT:
            d->imm = (((w >> 8) & BITMSK_12) << 4) + (w & 0b1111);
            return 0;

        case I_BX: case I_BXJ: case I_BLX:
            d->Rm = w & 0b1111;
            return 0;

        case I_MSR:
            d->Rn = w & 0b1111;
            d->imm = (w >> 18) & 0b11;
            return 0;

        case I_QSUB: case I_SMLAW: case I_SMULW: default:
            // returns -1
            break;
        }
        break;

    case T_MOV_IMM:
        d->Rd = (w >> 12) & 0b1111;
        d->imm = w & BITMSK_12;

        // the MOV and MVN instructions have an S bit
        if(d->instr == I_MOV || d->instr == I_MVN) {
            d->S = (w >> 20) & 1;
        }
        // the MOVW and the MOVT instructions take another 4 bits of immediate
        else {
            d->imm |= ((w >> 16) & 0b1111) << 12;
        }
        return 0;

    case T_CMP_OP:
        d->Rn = (w >> 16) & 0b1111;
        d->Rm = w & 0b1111;
        d->type = (w >> 5) & 0b11;

        // type == 1, shift with the value of the lower bits of Rs
        d->shift_is_reg = (w >> 4) & 1;
        if(d->shift_is_reg != 0) {
            d->Rs = (w >> 8) & 0b1111;
        }
        else {
            d->shift = (w >> 7) & 0b11111;
        }
        return 0;

    case T_CMP_IMM:
        d->Rn = (w >> 16) & 0b1111;
        d->imm = w & BITMSK_12;
        return 0;

    case T_OPLESS:
        d->instr = type_opless_instr_lookup[w & 0b111];
        return d->instr == I_INVLD ? -1 : 0;

    case T_DST_SRC:
        d->instr = type_shift_instr_lookup[(w >> 4) & 0b1111];
        if(d->instr != I_INVLD) {
            d->S = (w >> 20) & 1;
            d->Rd = (w >> 12) & 0b1111;
            d->type = (w >> 5) & 0b11;
            if((w >> 4) & 1) {
                d->Rm = (w >> 8) & 0b1111;
                d->Rn = w & 0b1111;
            }
            else {
                d->Rm = w & 0b1111;
                d->shift = (w >> 7) & 0b11111;

                // if this is a LSL instruction with a zero shift, then it's
                // actually a MOV instruction
                if(d->instr == I_LSL && d->type == 0 && d->shift == 0) {
                    d->instr = I_MOV;

                    // if Rd and Rm are equal, then this is a NOP instruction
                    // (although the manual only specifies if both are zero)
                    if(d->Rd == d->Rm) {
                        d->instr = I_NOP;
                    }
                }

                // if this is a ROR instruction with a zero shift, then it's
                // actually a RRX instruction
                else if(d->instr == I_ROR && d->type == 0b11 &&
                        d->shift == 0) {
                    d->instr = I_RRX;
                }
            }

            return 0;
        }

        // fall-through for all STR instructions
    }
    return -1;
}

int armv7_disassemble(darm_t *d, uint32_t w)
{
    int ret = -1;

    // clear the entire darm state, in order to make sure that no members
    // contain undefined data
    memset(d, 0, sizeof(darm_t));

    d->cond = (w >> 28) & 0b1111;
    d->w = w;

    if(d->cond == 0b1111) {
        // TODO handle unconditional instructions
    }
    else {
        ret = armv7_disas_cond(d, w);
    }

    // return error
    if(ret < 0) return ret;

    // TODO

    return 0;
}

const char *armv7_mnemonic_by_index(armv7_instr_t instr)
{
    return instr < ARRAYSIZE(armv7_mnemonics) ?
        armv7_mnemonics[instr] : NULL;
}

const char *armv7_enctype_by_index(armv7_enctype_t enctype)
{
    return enctype < ARRAYSIZE(armv7_enctypes) ?
        armv7_enctypes[enctype] : NULL;
}

const char *armv7_register_by_index(darm_reg_t reg)
{
    return reg < ARRAYSIZE(armv7_registers) ? armv7_registers[reg] : NULL;
}
