/* PlasmaAudio additions to libopus (not upstream): the run-time CPU detection's architecture, and a cap for it.
   The architecture levels on x86 are 0 = plain C, 1 = SSE, 2 = SSE2, 3 = SSE4.1, 4 = AVX2. Without run-time CPU
   detection (other platforms) the functions are not defined; check PL_OPUS_HAS_ARCH_CAP. */

#ifndef PLASMA_OPUS_ARCH_H
#define PLASMA_OPUS_ARCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* the highest level coders created from now on use; defined in celt/x86/x86cpu.c */
extern int opus_plasma_max_arch;

/* the level the CPU supports */
int opus_plasma_detected_arch(void);

#ifdef __cplusplus
}
#endif

#endif
