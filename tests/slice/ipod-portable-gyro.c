/* Direct host angular rate reaches the production L3G output registers,
 * including both actual board mounts and guest-selected sensitivity.
 * SLICE:lis include/hw/arm/ipod_touch_lis302dl.h typedef LIS302DLState
 * SLICE:lis include/hw/arm/ipod-attitude.h file
 * SLICE hw/arm/ipod_touch_lis302dl.c fn lis302dl_moving lis302dl_clear_host_motion lis302dl_set_host_motion lis302dl_motion_rate
 * SLICE hw/arm/s5l8930_i2c.c define L3G_CTRL5|L3G_OUT_X_L|L3G_FIFO_CTRL
 * SLICE hw/arm/s5l8930_i2c.c fn l3g_measure l3g_mode l3g_load_out
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#define CLAMP(x,lo,hi) ((x)<(lo)?(lo):(x)>(hi)?(hi):(x))
typedef int I2CSlave;
#define QEMU_CLOCK_VIRTUAL 0
static int64_t now=1000000000;
static int64_t qemu_clock_get_ns(int clock){return now;}
static void stw_le_p(uint8_t *p,int16_t value){p[0]=value;p[1]=(uint16_t)value>>8;}
#include "lis.h"
typedef struct {uint8_t regs[64],level;int16_t rate[3],fifo[96];char *dt_orientation;LIS302DLState *accel;} S5L8930L3GState;
/* LIS type must precede the minimal L3G shell. */
#include "slice.h"
static int16_t out(S5L8930L3GState *g,int axis){uint8_t *p=g->regs+0x28+axis*2;return (int16_t)(p[0]|p[1]<<8);}
int main(void){
 LIS302DLState accel={.motion_start_ns=-1};
 double a[3]={0,9.80665,0},w[3]={.125,-.25,.5};
 lis302dl_set_host_motion(&accel,3,a,w);
 const char *mounts[]={"00ff0000ff0000000000ff00","010000000001000000000100"}; /* n81, n90 DT gyro orientation */
 int maps[2][3]={{-2,-1,-3},{1,2,3}};
 double sensitivities[]={8.75,17.5,70,70};
 for(int board=0;board<2;board++)for(int fs=0;fs<4;fs++){
  S5L8930L3GState gyro={.dt_orientation=(char*)mounts[board],.accel=&accel};
  gyro.regs[0x23]=fs<<4;l3g_measure(&gyro,now);l3g_load_out(&gyro);
  for(int axis=0;axis<3;axis++){
   int mount=maps[board][axis];double r=w[abs(mount)-1]*(mount<0?-1:1);
   int expected=lround(r*180/M_PI*1000/sensitivities[fs]);
   assert(out(&gyro,axis)==expected && gyro.rate[axis]==expected);
  }
  l3g_measure(&gyro,now+250000000);l3g_load_out(&gyro);
  for(int axis=0;axis<3;axis++)assert(!out(&gyro,axis));
 }
 lis302dl_clear_host_motion(&accel);assert(!accel.host_fields);
 puts("PASS: production SI rad/s -> L3G signed little-endian guest output registers, n81/n90 mounts, all sensitivities, stale expiry/reset");
}
