#include "../KICQ/src/bart_codec.h"
#include "../KICQ/src/bart_wire.h"
#include <cassert>
int main() {
 bart::AssetId id; id.type=1; id.hash=std::string("0123456789abcdef",16);
 bart::Bytes payload;
 assert(bart::makeDownload("12345",id,payload));
 bart::SnacFrame packet{0x10,0x06,0,0,payload};
 bart::Bytes encoded;
 assert(bart::makeSnac(packet,encoded));
 bart::SnacFrame decoded;
 assert(bart::parseSnac(encoded,decoded));
 assert(decoded.family==0x10 && decoded.subtype==0x06 && decoded.payload==payload);
}
