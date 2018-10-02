/* spectrum.c: Generic Spectrum routines
   Copyright (c) 1999-2016 Philip Kendall, Darren Salt

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

#include "compat.h"
#include "debugger/debugger.h"
#include "display.h"
#include "event.h"
#include "keyboard.h"
#include "infrastructure/startup_manager.h"
#include "loader.h"
#include "machine.h"
#include "memory_pages.h"
#include "module.h"
#include "peripherals/printer.h"
#include "peripherals/ula.h"
#include "phantom_typist.h"
#include "psg.h"
#include "profile.h"
#include "rzx.h"
#include "settings.h"
#include "sound.h"
#include "spectrum.h"
#include "tape.h"
#include "timer/timer.h"
#include "ui/ui.h"
#include "ui/uijoystick.h"
#include "z80/z80.h"

// 1040 KB of RAM
libspectrum_byte RAM[ SPECTRUM_RAM_PAGES ][0x4000];

/* How many tstates have elapsed since the last interrupt? (or more
   precisely, since the ULA last pulled the /INT line to the Z80 low) */
libspectrum_dword tstates;

// Contention patterns
static int contention_pattern_65432100[] = { 5, 4, 3, 2, 1, 0, 0, 6 };
static int contention_pattern_76543210[] = { 5, 4, 3, 2, 1, 0, 7, 6 };

// Event
int spectrum_frame_event;

// Debugger variable prefix
static const char * const debugger_type_string = "spectrum";

// Debugger variable for frame count
static const char * const frame_count_name = "frames";

// Count of frames since last reset
static libspectrum_dword frames_since_reset;

static void
spectrum_reset( int hard_reset )
{
  frames_since_reset = 0;
}

static module_info_t module_info = {
  /* .reset = */ spectrum_reset,
  /* .romcs = */ NULL,
  /* .snapshot_enabled = */ NULL,
  /* .snapshot_from = */ NULL,
  /* .snapshot_to = */ NULL
};

static void
spectrum_frame_event_fn( libspectrum_dword last_tstates, int type,
			 void *user_data )
{
  if( rzx_playback ) event_force_events();
  rzx_frame();
  psg_frame();
  spectrum_frame();
  z80_interrupt();
  ui_joystick_poll();
  timer_estimate_speed();
  debugger_add_time_events();
  ui_event();
  ui_error_frame();
}

static libspectrum_dword
get_frame_count( void )
{
  return frames_since_reset;
}

static int
spectrum_init( void *context )
{
  spectrum_frame_event = event_register( spectrum_frame_event_fn,
					 "End of frame" );

  module_register( &module_info );

  debugger_system_variable_register( debugger_type_string,
      frame_count_name, get_frame_count, NULL );

  return 0;
}

void
spectrum_register_startup( void )
{
  startup_manager_module dependencies[] = {
    STARTUP_MANAGER_MODULE_DEBUGGER,
    STARTUP_MANAGER_MODULE_EVENT,
    STARTUP_MANAGER_MODULE_SETUID,
  };
  startup_manager_register( STARTUP_MANAGER_MODULE_SPECTRUM, dependencies,
                            ARRAY_SIZE( dependencies ), spectrum_init, NULL,
                            NULL );
}

int
spectrum_frame( void )
{
  libspectrum_dword frame_length;

  /* Reduce the t-state count of both the processor and all the events
     scheduled to occur. Done slightly differently if RZX playback is
     occurring */
  frame_length = rzx_playback ? tstates
			      : machine_current->timings.tstates_per_frame;

  event_frame( frame_length );
  debugger_breakpoint_reduce_tstates( frame_length );
  tstates -= frame_length;
  if( z80.interrupts_enabled_at >= 0 )
    z80.interrupts_enabled_at -= frame_length;

  if( sound_enabled ) sound_frame();

  if( display_frame() ) return 1;
  if( profile_active ) profile_frame( frame_length );
  printer_frame();

  // Add an interrupt unless they're being generated by .rzx playback
  if( !rzx_playback )
    event_add( machine_current->timings.tstates_per_frame,
               spectrum_frame_event );

  loader_frame( frame_length );
  phantom_typist_frame();

  frames_since_reset++;

  return 0;
}

libspectrum_byte
spectrum_contend_delay_none( libspectrum_dword time )
{
  return 0;
}

static libspectrum_byte
contend_delay_common( libspectrum_dword time, int* timings, int offset )
{
  int line, tstates_through_line;

  line =
    (libspectrum_signed_dword)( time - machine_current->line_times[ 0 ] ) /
    machine_current->timings.tstates_per_line;

  /* Work out where we are in this line, remembering that line_times[0] holds
     the first pixel we display, not the start of where the Spectrum produced
     the left border */
  tstates_through_line = time - machine_current->line_times[ 0 ] +
    ( machine_current->timings.left_border - DISPLAY_BORDER_WIDTH_COLS * 4 );

  tstates_through_line %= machine_current->timings.tstates_per_line;

  // No contention in the upper and lower borders
  if( line < DISPLAY_BORDER_HEIGHT                   ||
      line >= DISPLAY_BORDER_HEIGHT + DISPLAY_HEIGHT    ) return 0;

  // Or in the left border
  if( tstates_through_line < machine_current->timings.left_border - offset )
    return 0;

  // Or the right border or retrace
  if( tstates_through_line >= machine_current->timings.left_border +
                              machine_current->timings.horizontal_screen -
                              offset )
    return 0;

  /* We now know the ULA is reading the screen, so put in the appropriate
     delay */
  return timings[ tstates_through_line % 8 ];
}

libspectrum_byte
spectrum_contend_delay_65432100( libspectrum_dword time )
{
  return contend_delay_common( time, contention_pattern_65432100, 1 );
}

libspectrum_byte
spectrum_contend_delay_76543210( libspectrum_dword time )
{
  return contend_delay_common( time, contention_pattern_76543210, 4 );
}

// What happens if we read from an unattached port?
libspectrum_byte
spectrum_unattached_port( void )
{
  int line, tstates_through_line, column;

  // Return 0xff (idle bus) if we're in the top border
  if( tstates < machine_current->line_times[ DISPLAY_BORDER_HEIGHT ] )
    return 0xff;

  // Work out which line we're on, relative to the top of the screen
  line = ( (libspectrum_signed_dword)tstates -
	   machine_current->line_times[ DISPLAY_BORDER_HEIGHT ] ) /
    machine_current->timings.tstates_per_line;

  // Idle bus if we're in the lower border
  if( line >= DISPLAY_HEIGHT ) return 0xff;

  /* Work out where we are in this line, remembering that line_times[] holds
     the first pixel we display, not the start of where the Spectrum produced
     the left border */
  tstates_through_line = tstates -
    machine_current->line_times[ DISPLAY_BORDER_HEIGHT + line ] +
    ( machine_current->timings.left_border - DISPLAY_BORDER_WIDTH_COLS * 4 );

  // Idle bus if we're in the left border
  if( tstates_through_line < machine_current->timings.left_border )
    return 0xff;

  // Or the right border or retrace
  if( tstates_through_line >= machine_current->timings.left_border +
                              machine_current->timings.horizontal_screen  )
    return 0xff;

  column = ( ( tstates_through_line -
	       machine_current->timings.left_border ) / 8 ) * 2;

  switch( tstates_through_line % 8 ) {

    /* The pattern of bytes returned here is the same as documented by
       Ramsoft in their 'Floating bus technical guide' at
       http://web.archive.org/web/20080509193736/http://www.ramsoft.bbk.org/floatingbus.html

       However, the timings used are based on the first byte being
       returned at 14338 (48K) and 14364 (128K) respectively, not
       14347 and 14368 as used by Ramsoft.

       In contrast to previous versions of this code, Arkanoid and
       Sidewize now work. */

    // Attribute bytes
    case 5: column++;
    case 3:
      return RAM[ memory_current_screen ][ display_attr_start[line] + column ];

    // Screen data
    case 4: column++;
    case 2:
      return RAM[ memory_current_screen ][ display_line_start[line] + column ];

    // Idle bus
    case 0: case 1: case 6: case 7:
      return 0xff;

  }

  return 0; // Keep gcc happy
}

libspectrum_byte
spectrum_unattached_port_none( void )
{
  return 0xff;
}
