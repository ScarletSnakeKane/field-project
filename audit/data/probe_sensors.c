#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ds18b20_decode.h"
#include "aht20_decode.h"
#include "soil_curve.h"
#include "battery_curve.h"
int main(void){
  unsigned char sp[9]; float t=-999,h=-999; int st;
  memset(sp,0xFF,9); st=DS18B20_DecodeScratchpad(sp,&t);
  printf("all FF: crc8=%02X st=%d t=%f\n", DS18B20_CRC8(sp,8), st, t);
  /* count random-ish single-byte nonzero frames passing CRC: e.g., first byte X rest 0 */
  int pass=0; for(int x=1;x<256;x++){memset(sp,0,9);sp[0]=x;sp[8]=DS18B20_CRC8(sp,8);t=-999; if(DS18B20_DecodeScratchpad(sp,&t)==0) pass++;}
  printf("frames with only temp LSB + valid crc accepted: %d\n",pass);
  /* AHT20: frame with hum zero, temp live */
  unsigned char f[6]={0x18,0,0,0x06,0,0}; t=h=-999; st=AHT20_DecodeFrame(f,&t,&h); printf("aht hum0: st=%d t=%f h=%f\n",st,t,h);
  /* status with CAL bit cleared but data nonzero */
  unsigned char g[6]={0x00,0x80,0x00,0x06,0x00,0x00}; st=AHT20_DecodeFrame(g,&t,&h); printf("aht nocal: st=%d\n",st);
  /* fraction of random frames accepted by range check */
  unsigned long acc=0,tot=0; for(unsigned r=0;r<(1u<<20);r+=7){ unsigned char q[6]={0x18,0x55,0x55,(unsigned char)(0x50|((r>>16)&0xF)),(unsigned char)(r>>8),(unsigned char)r}; tot++; if(AHT20_DecodeFrame(q,&t,&h)==0) acc++;}
  printf("aht random temp accepted: %.1f%%\n",100.0*acc/tot);
  printf("soil(0V)=%f soil(3.3V)=%f soil(NaN)=%f\n",Soil_VoltageToMoisture(0),Soil_VoltageToMoisture(3.3f),Soil_VoltageToMoisture(NAN));
  printf("bat(NaN)=%u\n",Battery_VoltsToPercent(NAN));
  return 0;}
