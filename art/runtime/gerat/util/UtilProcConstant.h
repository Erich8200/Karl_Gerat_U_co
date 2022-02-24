#ifndef ART_RUNTIME_TWODROID_CONSTANT_H_
#define ART_RUNTIME_TWODROID_CONSTANT_H_

#include <stdint.h>

typedef unsigned int 	u4;
typedef unsigned short	u2;
typedef unsigned char	u1;
typedef long long		s8;


const static u4	MaxLineLen			= 256;
const static u4 ProcFileNameMaxLen	= 64;

union myunion{
	double d;
	u4 u[2];
	s8 s;
};

#endif