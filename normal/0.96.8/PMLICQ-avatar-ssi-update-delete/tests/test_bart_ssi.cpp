#include "../KICQ/src/bart_ssi.h"
#include <cassert>
int main() {
 std::string out;
 assert(!bartssi::iconItem(0,"1234567890123456",out));
 assert(!bartssi::iconItem(123,"short",out));
 assert(bartssi::iconItem(0x4000,"1234567890123456",out));
 assert(out.size()==34);
 assert((unsigned char)out[5]==0x00);
 assert((unsigned char)out[6]==0x00 && (unsigned char)out[7]==0x14);
 assert((unsigned char)out[8]==0x00 && (unsigned char)out[9]==0x16);
 assert(out.substr(18)=="1234567890123456");
 assert(bartssi::deleteItem(0x4000,out));
 assert(out.size()==10);
 assert((unsigned char)out[6]==0 && (unsigned char)out[7]==0x14);
}
