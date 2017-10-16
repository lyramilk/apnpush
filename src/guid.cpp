#include "guid.h"
#include <iomanip>

guid::guid()
{
	h = l = 0;
}

guid::~guid()
{}

guid& guid::operator++()
{
	if(l == 0xffffffffffffffff){
		l = 0;
		++h;
	}else{
		++l;
	}
	return *this;
}

guid guid::operator++(int)
{
	guid g = *this;
	++g;
	return g;
}

lyramilk::data::string guid::str()
{
	lyramilk::data::stringstream ss;
	ss << std::hex << std::setw(16) << std::setfill('0') << h << std::hex << std::setw(16) << std::setfill('0') << l;
	lyramilk::data::string str = ss.str();
	lyramilk::data::string str2;
	str2.reserve(str.size() + 4);
	str2.append(str.substr(0,8));
	str2.push_back('-');
	str2.append(str.substr(8,4));
	str2.push_back('-');
	str2.append(str.substr(12,4));
	str2.push_back('-');
	str2.append(str.substr(16,4));
	str2.push_back('-');
	str2.append(str.substr(20,12));
	return str2;
}

lyramilk::data::string guid::str(unsigned long long l)
{
	guid g;
	g.l = l;
	return g.str();
}

lyramilk::data::string guid::str(unsigned long long h,unsigned long long l)
{
	guid g;
	g.l = l;
	g.h = h;
	return g.str();
}
