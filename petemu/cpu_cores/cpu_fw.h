#pragma once
#include "..//deftypes.h"


struct MemoryWriteByte
{
	unsigned int lowAddr;
	unsigned int highAddr;
	void(*memoryCall)(unsigned int, unsigned char, struct MemoryWriteByte*);
	void* pUserArea;
};

struct MemoryReadByte
{
	unsigned int lowAddr;
	unsigned int highAddr;
	unsigned char(*memoryCall)(unsigned int, struct MemoryReadByte*);
	void* pUserArea;
};

struct z80PortWrite
{
	UINT16 lowIoAddr;
	UINT16 highIoAddr;
	void(*IOCall)(UINT16, UINT8, struct z80PortWrite*);
	void* pUserArea;
};

struct z80PortRead
{
	UINT16 lowIoAddr;
	UINT16 highIoAddr;
	UINT16(*IOCall)(UINT16, struct z80PortRead*);
	void* pUserArea;
};