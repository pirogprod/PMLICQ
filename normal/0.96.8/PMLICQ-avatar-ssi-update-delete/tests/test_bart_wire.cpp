#include "../KICQ/src/bart_wire.h"
#include <cassert>
#include <iostream>
int main(){
 bart::SnacFrame s; s.family=0x10;s.subtype=6;s.requestId=0x12345678;s.payload="abc";
 std::string encoded; assert(bart::makeSnac(s,encoded));
 bart::SnacFrame decoded;assert(bart::parseSnac(encoded,decoded));
 assert(decoded.family==16&&decoded.subtype==6&&decoded.requestId==0x12345678&&decoded.payload=="abc");
 bart::FlapFrame f{2,17,encoded};std::string frame;assert(bart::makeFlap(f,frame));
 std::string b=frame.substr(0,4);bart::FlapFrame received;assert(bart::consumeFlap(b,received)==0);
 b+=frame.substr(4);assert(bart::consumeFlap(b,received)==1);assert(b.empty());
 assert(received.sequence==17&&received.payload==encoded);
 std::string login;assert(bart::makeCookieLogin("token",1,login));
 assert(login[1]==1&&bart::read16(login,10)==6&&bart::read16(login,12)==5);
 std::cout<<"BART FLAP/SNAC codec tests passed\n";
}
