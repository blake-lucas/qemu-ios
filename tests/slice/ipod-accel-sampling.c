/* Production sensor sampling: rate limits, bounded noise, shake and restore, held host samples.
 *
 * SLICE include/hw/arm/ipod_touch_lis302dl.h define ACCEL_WHOAMI_VALUE|ACCEL_CTRL_REG1_FS
 * SLICE include/hw/arm/ipod_touch_lis302dl.h typedef LIS302DLState
 * SLICE include/hw/arm/ipod-attitude.h file
 * SLICE hw/arm/ipod_touch_lis302dl.c fn lis302dl_counts lis302dl_mounted lis302dl_moving lis302dl_gravity
 * SLICE hw/arm/ipod_touch_lis302dl.c fn lis302dl_clear_host_motion lis302dl_set_host_motion lis302dl_motion_rate lis302dl_sample lis302dl_post_load
 */
#include <stdint.h>
#include <math.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define QEMU_CLOCK_VIRTUAL 0
static int64_t sensor_now;
static int64_t qemu_clock_get_ns(int clock) {return sensor_now;}
#define CLAMP(x, lo, hi) ((x)<(lo)?(lo):(x)>(hi)?(hi):(x))
typedef int I2CSlave;
#include "slice.h"

int main(void) {
 LIS302DLState s={.base_y=-64,.last_sample_ns=-1,.shake_start_ns=-1};
 lis302dl_sample(&s,0);
 int x=s.out_x,y=s.out_y,z=s.out_z;uint32_t rng=s.noise_state;
 s.base_x=32;s.base_y=0;
 lis302dl_sample(&s,9999999);
 assert(s.out_x==x&&s.out_y==y&&s.out_z==z&&s.noise_state==rng);
 lis302dl_sample(&s,10000000);assert(abs(s.out_x-32)<=1&&abs(s.out_y)<=1);
 s.ctrl_reg1=0x80;
 rng=s.noise_state;lis302dl_sample(&s,12499999);assert(s.noise_state==rng);
 lis302dl_sample(&s,12500000);assert(s.noise_state!=rng);
 s.rate_hz=50;rng=s.noise_state;
 lis302dl_sample(&s,32499999);assert(s.noise_state==rng);
 lis302dl_sample(&s,32500000);assert(s.noise_state!=rng);
 s.rate_hz=100;s.base_x=127;s.base_y=-128;s.base_z=0;
 int seen[3]={0};
 for(int i=1;i<=10000;i++) {
  lis302dl_sample(&s,32500000LL+i*10000000LL);
  assert(s.out_x>=126 && s.out_x<=127 && s.out_y>=-128 && s.out_y<=-127);
  assert(abs(s.out_z)<=1);seen[s.out_z+1]++;
 }
 assert(seen[0]>2500&&seen[1]>2500&&seen[2]>2500);
 s.base_x=s.base_y=s.base_z=0;s.last_sample_ns=-1;
 lis302dl_sample(&s,0);assert(!s.out_x&&!s.out_y&&!s.out_z);
 s.shake_start_ns=10000000;s.last_sample_ns=-1;
 for(int i=0;i<20;i++) {
  lis302dl_sample(&s,10000000LL+i*10000000LL);
  int sign=(i/2)&1?-1:1;
  assert(s.out_x==127*sign&&s.out_y==-127*sign&&s.out_z==63*sign);
 }
 lis302dl_sample(&s,210000000);assert(!s.out_x&&!s.out_y&&!s.out_z&&s.shake_start_ns==-1);
 s.shake_start_ns=500;s.rate_hz=50;rng=s.noise_state;
 assert(!lis302dl_post_load(&s,3));
 assert(s.shake_start_ns==-1&&s.last_sample_ns==-1&&s.rate_hz==50&&s.noise_state==rng);
 s.rate_hz=401;assert(lis302dl_post_load(&s,3)==-EINVAL);
 assert(!lis302dl_post_load(&s,2)&&s.rate_hz==0&&s.noise_state);
 /* The LIS302DL itself: 18 mg/digit at +-2 g (1 g = 55.6 counts), 72 mg at +-8 g (FS). */
 LIS302DLState part={.whoami=0x3B,.base_z=-64,.last_sample_ns=-1,.shake_start_ns=-1};
 lis302dl_sample(&part,0);assert(abs(part.out_z+56)<=1&&abs(part.out_x)<=1&&abs(part.out_y)<=1);
 part.ctrl_reg1=0x20;part.last_sample_ns=-1;
 lis302dl_sample(&part,0);assert(abs(part.out_z+14)<=1);
 part.ctrl_reg1=0;part.shake_start_ns=0;part.last_sample_ns=-1;
 lis302dl_sample(&part,0);assert(part.out_x==110&&part.out_y==-110&&part.out_z==55);
 /* Direct Android/Linux samples use SI units, preserve board mount, and
  * the acceleration holds while the host is quiet (a gap between samples is not the device turning back to
  * its resting pose), the rotation rate lapses to zero, and letting go returns the resting pose. */
 LIS302DLState live={.whoami=0x3B,.last_sample_ns=-1,.shake_start_ns=-1,.motion_start_ns=-1,
                     .dt_orientation=(char *)"0b000000"};
 double a[3]={0,9.80665,0},w[3]={.5,-.25,1.25},got[3];
 sensor_now=1000000000;lis302dl_set_host_motion(&live,3,a,w);
 lis302dl_sample(&live,sensor_now);
 assert(abs(live.out_x-56)<=1 && abs(live.out_y)<=1 && abs(live.out_z)<=1);
 assert(lis302dl_motion_rate(&live,sensor_now,got));
 for(int i=0;i<3;i++)assert(got[i]==w[i]);
 assert(!lis302dl_motion_rate(&live,sensor_now+250000000,got));
 for(int i=0;i<3;i++)assert(got[i]==0);
 for(int64_t later=250000000;later<=10000000000LL;later+=250000000){
  live.last_sample_ns=-1;lis302dl_sample(&live,sensor_now+later);
  assert(abs(live.out_x-56)<=1 && abs(live.out_y)<=1 && abs(live.out_z)<=1);
 }
 lis302dl_clear_host_motion(&live);
 live.last_sample_ns=-1;lis302dl_sample(&live,sensor_now+10000000000LL);
 assert(!live.out_x&&!live.out_y&&!live.out_z);
 lis302dl_set_host_motion(&live,3,a,w);assert(live.host_fields==3);
 lis302dl_post_load(&live,3);assert(!live.host_fields);
 puts("PASS: 100/400 Hz DR, explicit rate, stable triplets, bounded noise, three-axis 200 ms shake, snapshot restore, LIS302DL 18/72 mg per count, host acceleration held between samples, rotation rate lapses");
}
