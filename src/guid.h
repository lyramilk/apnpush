#ifndef _mpush_guid_h_
#define _mpush_guid_h_
#include "config.h"
#include <libmilk/var.h>

class guid
{
	unsigned long long h;
	unsigned long long l;
  public:
	guid();
	~guid();
	guid& operator++();
	guid operator++(int);
	lyramilk::data::string str();
	lyramilk::data::string static str(unsigned long long l);
	lyramilk::data::string static str(unsigned long long h,unsigned long long l);
};

#endif