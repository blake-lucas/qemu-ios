/* LT_SAMPLE: the vCPU threads' CPU-time sampler that runs with a profile capture (hw/arm/it-sampler.c). */
#ifndef HW_ARM_IT_SAMPLER_H
#define HW_ARM_IT_SAMPLER_H

void it_sampler_start(void);
/* Writes CAPTURE.samples. */
void it_sampler_stop(const char *capture);

#endif
