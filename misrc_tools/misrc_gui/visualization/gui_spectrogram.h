/*
 * MISRC GUI - Real-time Spectrogram / Waterfall panel module
 *
 * Provides two FFT-derived scrolling 2D color views that share one engine:
 *
 *   Waterfall    - frequency on X, time scrolling down, newest at top
 *                  (SDR / radio convention).
 *   Spectrograph - time on X scrolling left, frequency on Y with low freq
 *                  at the bottom and high freq at the top (audio / speech
 *                  spectrogram convention).
 *
 * Both reuse the existing FFT engine (gui_fft_process_raw / fft_state_t) for
 * the per-frame magnitude spectrum, then map magnitude to the same heatmap
 * ramp used by the GPU phosphor compositor (black->blue->cyan->green->
 * yellow->red) so the colors match the rest of the application.
 *
 * Threading:
 *   - process() runs on the display thread (no OpenGL) and only computes the
 *     FFT into the embedded fft_state_t back buffer.
 *   - render() runs on the render/main thread (OpenGL available); it consumes
 *     the latest magnitude frame, appends it to a CPU history buffer, uploads
 *     to a Texture2D and draws it.
 *
 * Requires FFTW3 single-precision (fftw3f), same as the FFT panel.
 */

#ifndef GUI_SPECTROGRAM_H
#define GUI_SPECTROGRAM_H

#include <stdbool.h>
#include "../core/gui_app.h"
#include "panel_interface.h"

//-----------------------------------------------------------------------------
// Panel Interface Registration
//-----------------------------------------------------------------------------

// Register the Waterfall panel vtable with the panel registry. Call once at startup.
void gui_waterfall_panel_register(void);

// Register the Spectrograph panel vtable with the panel registry. Call once at startup.
void gui_spectrograph_panel_register(void);

//-----------------------------------------------------------------------------
// Availability
//-----------------------------------------------------------------------------

// Returns true if the spectrogram panels are available (requires FFTW, like the FFT panel).
bool gui_spectrogram_available(void);

#endif // GUI_SPECTROGRAM_H
