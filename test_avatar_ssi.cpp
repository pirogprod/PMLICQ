#include "normal/0.96.7/KICQ/src/bart_ssi.h"
#include <cassert>
#include <cstdint>
#include <iostream>
static unsigned be16(const std::string& s,size_t i) { return (uint8_t(s[i])<<8)|uint8_t(s[i+1]); }
int main() {
 const std::string hash("0123456789ABCDEF",16); std::string x;
 assert(bartssi::iconItem(0x4000, hash, x));
 assert(x.size()==33); // 2 name-len + 1 name + 2 group + 2 item + 2 type + 2 TLV-len + 4 TLV header + 18 bytes
 assert(be16(x,0)==1 && x[2]=='1');
 assert(be16(x,3)==0 && be16(x,5)==0x4000 && be16(x,7)==0x14);
 assert(be16(x,9)==22 && be16(x,11)==0xD5 && be16(x,13)==18);
 assert(uint8_t(x[15])==1 && uint8_t(x[16])==16);
 assert(x.substr(17,16)==hash);
 std::string y; assert(bartssi::deleteItem(0x4000,y));
 assert(y.size()==11 && y.substr(0,9)==x.substr(0,9) && be16(y,9)==0);
 std::cout << "PASS SSI icon add/update and delete lengths, name, MD5\n";
}
