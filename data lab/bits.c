/* 
 *  학번 : 201201356           
 *  이름 : 김민호 
 */

#include "btest.h"
#include <limits.h>

int bitAnd(int x, int y) {
	return ~(~x|~y);
}

int getByte(int x, int n) {
	return (x>>(n<<3))&0xff;
}

int logicalShift(int x, int n) {
	return (x>>n)&~(~0<<(33+~n));
}

int bitCount(int x) {
	int temp1, temp2, temp3, temp4, temp5;
	temp1 = (0x55<<8)|0x55;
	temp1 = (temp1<<16)|temp1;
	temp2 = (0x33<<8)|0x33;
	temp2 = (temp2<<16)|temp2;
	temp3 = (0x0f<<8)|0x0f;
	temp3 = (temp3<<16)|temp3;
	temp4 = (0xff<<16)|0xff;
	temp5 = (0xff<<8)|0xff;

	x = (x&temp1) + ((x>>1)&temp1);
	x = (x&temp2) + ((x>>2)&temp2);
	x = (x&temp3) + ((x>>4)&temp3);
	x = (x&temp4) + ((x>>8)&temp4);
	x = (x&temp5) + ((x>>16)&temp5);
	return x;
}

int tmin(void) {
	return 1<<31; 
}

int fitsBits(int x, int n) {
	int temp = 33 + ~n;
	return !(((x<<temp)>>temp)^x);
}

int isPositive(int x) {
	return !((x>>31)+!x);
}
