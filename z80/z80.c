/* z80.c: z80 supplementary functions
   Copyright (c) 1999-2016 Philip Kendall
   Copyright (c) 2015 Stuart Brady

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

   Author contact information:

   E-mail: philip-fuse@shadowmagic.org.uk

*/

#include <config.h>

#include <libspectrum.h>

#include "debugger/debugger.h"
#include "event.h"
#include "fuse.h"
#include "infrastructure/startup_manager.h"
#include "memory_pages.h"
#include "module.h"
#include "peripherals/scld.h"
#include "peripherals/spectranet.h"
#include "rzx.h"
#include "settings.h"
#include "spectrum.h"
#include "ui/ui.h"
#include "z80.h"
#include "z80_internals.h"
#include "z80_macros.h"

/* Whether a half carry occurred or not can be determined by looking at the 3rd bit of the two arguments and the result;
   these are hashed into this table in the form r12, where r is the 3rd bit of the result,
   1 is the 3rd bit of the 1st argument and 2 is the third bit of the 2nd argument;
   the tables differ for add and subtract operations */
const libspectrum_byte halfcarry_add_table[] = {0, FLAG_H, FLAG_H, FLAG_H, 0, 0, 0, FLAG_H};
const libspectrum_byte halfcarry_sub_table[] = {0, 0, FLAG_H, 0, FLAG_H, 0, FLAG_H, FLAG_H};

// Similarly, overflow can be determined by looking at the 7th bits; again the hash into this table is r12
const libspectrum_byte overflow_add_table[] = {0, 0, 0, FLAG_V, FLAG_V, 0, 0, 0};
const libspectrum_byte overflow_sub_table[] = {0, FLAG_V, 0, 0, 0, 0, FLAG_V, 0};

// Some more tables; initialised in z80_init_tables()

libspectrum_byte sz53_table[0x100]; // The S, Z, 5 and 3 bits of the index
libspectrum_byte parity_table[0x100]; // The parity of the lookup value
libspectrum_byte sz53p_table[0x100]; // OR the above two tables together

// This is what everything acts on!
processor z80;

int z80_interrupt_event;
int z80_nmi_event;
int z80_nmos_iff2_event;

static void z80_init_tables(void);
static void z80_from_snapshot(libspectrum_snap *snap);
static void z80_to_snapshot(libspectrum_snap *snap);
static void z80_nmi(libspectrum_dword ts, int type, void *user_data);

static module_info_t z80_module_info = {
    z80_reset,
    NULL,
    NULL,
    z80_from_snapshot,
    z80_to_snapshot
};


static void z80_interrupt_event_fn(libspectrum_dword event_tstates, int type, void *user_data)
{
    // Retriggered interrupt; firstly, ignore if we're doing RZX playback as all interrupts are generated by the RZX code
    if (rzx_playback) {
        return;
    }

    // Otherwise, see if we actually accept an interrupt. If we do and we're doing RZX recording, store a frame
    if (z80_interrupt()) rzx_frame();
}


// Set up the z80 emulation
int z80_init(void *context)
{
    z80_init_tables();

    z80_interrupt_event = event_register(z80_interrupt_event_fn, "Retriggered interrupt");
    z80_nmi_event = event_register(z80_nmi, "Non-maskable interrupt");
    z80_nmos_iff2_event = event_register(NULL, "IFF2 update dummy event");

    module_register(&z80_module_info);

    z80_debugger_variables_init();

    return 0;
}


void z80_register_startup(void)
{
    startup_manager_module dependencies[] = {
        STARTUP_MANAGER_MODULE_DEBUGGER,
        STARTUP_MANAGER_MODULE_EVENT,
        STARTUP_MANAGER_MODULE_SETUID
    };
    startup_manager_register(STARTUP_MANAGER_MODULE_Z80, dependencies, ARRAY_SIZE(dependencies), z80_init, NULL, NULL);
}


// Initalise the tables used to set flags
static void z80_init_tables(void)
{
    int i,j,k;
    libspectrum_byte parity;

    for (i = 0; i < 0x100; i++) {
        sz53_table[i]= i & (FLAG_3 | FLAG_5 | FLAG_S);
        j = i;
        parity = 0;
        for (k = 0; k < 8; k++) {
            parity ^= j & 1;
            j >>= 1;
        }
        parity_table[i] = (parity ? 0 : FLAG_P);
        sz53p_table[i] = sz53_table[i] | parity_table[i];
    }

    sz53_table[0] |= FLAG_Z;
    sz53p_table[0] |= FLAG_Z;

}


// Reset the z80
void z80_reset(int hard_reset)
{
    AF = AF_ = 0xffff;
    I = R = R7 = 0;
    PC = 0;
    SP = 0xffff;
    IFF1 = IFF2 = IM = 0;
    z80.halted = 0;
    z80.iff2_read = 0;
    Q = 0;

    if (hard_reset) {
        BC  = DE  = HL  = 0;
        BC_ = DE_ = HL_ = 0;
        IX = IY = 0;
        z80.memptr.w = 0; // TODO: confirm if this happens on soft reset
    }

    z80.interrupts_enabled_at = -1;
}


// Process a z80 maskable interrupt
int z80_interrupt(void)
{
    /* An interrupt will occur if IFF1 is set and the /INT line hasn't gone high again.
       On a Timex machine, we also need the SCLD's INTDISABLE to be clear */
    if (IFF1 && (tstates < machine_current->timings.interrupt_length) && !scld_last_dec.name.intdisable) {

        if (z80.iff2_read && !IS_CMOS) {
            /* We just executed LD A,I or LD A,R, causing IFF2 to be copied to the parity flag.
               This occured whilst accepting an interrupt.
               For NMOS Z80s only, clear the parity flag to reflect the fact that
               IFF2 would have actually been cleared before its value was transferred by LD A,I or LD A,R.
               We cannot do this when emulating LD itself as we cannot tell whether the next instruction will be interrupted. */
            F &= ~FLAG_P;
        }

        /* If interrupts have just been enabled, don't accept the interrupt now,
           but check after the next instruction has been executed */
        if (tstates == z80.interrupts_enabled_at) {
            event_add((tstates + 1), z80_interrupt_event);
            return 0;
        }

        if (z80.halted) {
            PC++;
            z80.halted = 0;
        }

        IFF1 = IFF2 = 0;
        R++;
        rzx_instructions_offset--;

        tstates += 7; // Longer than usual M1 cycle

        writebyte(--SP, PCH);
        writebyte(--SP, PCL);

        switch (IM) {

            case 0:
                /* We assume 0xff (RST 38) is on the data bus, as the Spectrum leaves
                   it pulled high when the end-of-frame interrupt is delivered.  Only
                   the first byte is provided directly to the Z80: all remaining bytes
                   of the instruction are fetched from memory using PC, which is
                   incremented as normal.  As RST 38 takes a single byte, we do not
                   emulate fetching of additional bytes. */
                PC = 0x0038;
                break;

            case 1:
                // RST 38
                PC = 0x0038;
                break;

            case 2:
                /* We assume 0xff is on the data bus, as the Spectrum leaves it pulled
                   high when the end-of-frame interrupt is delivered.
                   Our interrupt vector is therefore 0xff. */
                ; // empty statement
                libspectrum_word inttemp = (0x100 * I) + 0xff;
                PCL = readbyte(inttemp++);
                PCH = readbyte(inttemp);
                break;

            default:
                ui_error(UI_ERROR_ERROR, "Unknown interrupt mode %d", IM);
                fuse_abort();
        }

        z80.memptr.w = PC;
        Q = 0;

        return 1; // Accepted an interrupt

    } else {

        return 0; // Did not accept an interrupt

    }
}


// Process a z80 non-maskable interrupt
static void z80_nmi(libspectrum_dword ts, int type, void *user_data)
{
    // TODO: this isn't ideal
    if (spectranet_available && spectranet_nmi_flipflop()) {
        return;
    }

    if (z80.halted) {
        PC++;
        z80.halted = 0;
    }

    IFF1 = 0;
    R++;
    tstates += 5;

    writebyte(--SP, PCH);
    writebyte(--SP, PCL);

    // TODO: check whether any of these should occur before PC is pushed.
    if (machine_current->capabilities & LIBSPECTRUM_MACHINE_CAPABILITY_SCORP_MEMORY) {

        // Page in ROM 2
        writeport_internal(0x1ffd, (machine_current->ram.last_byte2 | 0x02));

    } else if (beta_available) {

        // Page in TR-DOS ROM
        beta_page();

    } else if (spectranet_available) {

        // Page in spectranet
        spectranet_nmi();

    }

    Q = 0;
    PC = 0x0066;
}


// Special peripheral processing for RETN
void z80_retn(void)
{
    spectranet_retn();
}


// Routines for transferring the Z80 contents to and from snapshots
static void z80_from_snapshot(libspectrum_snap *snap)
{
    A = libspectrum_snap_a (snap);
    F = libspectrum_snap_f (snap);
    A_ = libspectrum_snap_a_(snap);
    F_ = libspectrum_snap_f_(snap);

    BC = libspectrum_snap_bc (snap);
    DE = libspectrum_snap_de (snap);
    HL = libspectrum_snap_hl (snap);
    BC_ = libspectrum_snap_bc_(snap);
    DE_ = libspectrum_snap_de_(snap);
    HL_ = libspectrum_snap_hl_(snap);

    IX = libspectrum_snap_ix(snap);
    IY = libspectrum_snap_iy(snap);
    I = libspectrum_snap_i (snap);
    R = R7 = libspectrum_snap_r(snap);
    SP = libspectrum_snap_sp(snap);
    PC = libspectrum_snap_pc(snap);

    IFF1 = libspectrum_snap_iff1(snap);
    IFF2 = libspectrum_snap_iff2(snap);
    IM = libspectrum_snap_im(snap);

    z80.memptr.w = libspectrum_snap_memptr(snap);

    z80.halted = libspectrum_snap_halted(snap);

    z80.interrupts_enabled_at = libspectrum_snap_last_instruction_ei(snap) ? tstates : -1;

    Q = libspectrum_snap_last_instruction_set_f(snap) ? F : 0;
}


static void z80_to_snapshot(libspectrum_snap *snap)
{
    libspectrum_byte r_register;

    r_register = (R7 & 0x80) | (R & 0x7f);

    libspectrum_snap_set_a(snap, A);
    libspectrum_snap_set_f(snap, F);
    libspectrum_snap_set_a_(snap, A_);
    libspectrum_snap_set_f_(snap, F_);

    libspectrum_snap_set_bc(snap, BC);
    libspectrum_snap_set_de(snap, DE);
    libspectrum_snap_set_hl(snap, HL);
    libspectrum_snap_set_bc_(snap, BC_);
    libspectrum_snap_set_de_(snap, DE_);
    libspectrum_snap_set_hl_(snap, HL_);

    libspectrum_snap_set_ix(snap, IX);
    libspectrum_snap_set_iy(snap, IY);
    libspectrum_snap_set_i(snap, I);
    libspectrum_snap_set_r(snap, r_register);
    libspectrum_snap_set_sp(snap, SP);
    libspectrum_snap_set_pc(snap, PC);

    libspectrum_snap_set_memptr(snap, z80.memptr.w);

    libspectrum_snap_set_iff1(snap, IFF1);
    libspectrum_snap_set_iff2(snap, IFF2);
    libspectrum_snap_set_im(snap, IM);

    libspectrum_snap_set_halted(snap, z80.halted);
    libspectrum_snap_set_last_instruction_ei(snap, (z80.interrupts_enabled_at == tstates));

    /* If last instruction set F but it's zero, it is saved as false, but the
       result of the next (hypothetically) SCF/CCF instruction it's independent of this flag */
    libspectrum_snap_set_last_instruction_set_f(snap, !!Q);
}
