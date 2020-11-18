#include <stdlib.h>
#include <inttypes.h>

//random number between rndL and rndH (rndL <= rnd <=rndH)
//only positive values
//user must provide correct arguments, such that 0 <= rndL < rndH

uint32_t randomNumber(uint32_t rndL, uint32_t rndH)
{
	uint32_t range = rndH + 1 - rndL;
	return rand() % range + rndL;
}
