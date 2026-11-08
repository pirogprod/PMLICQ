#include "../KICQ/src/bart_avatar_store.h"
#include <cassert>
#include <fstream>
#include <iostream>
int main(int argc, char **argv) {
 assert(argc > 1);
 bart::ImageReply reply;
 reply.screenName="454111";
 reply.id.type=1;
 reply.id.hash=std::string("\x00\xff\x33",3);
 reply.image=std::string("GIF89a",6)+"abcdef";
 std::string path;
 assert(bart::cacheImage(reply, argv[1], path));
 assert(path.find("00ff33.gif")!=std::string::npos);
 std::ifstream f(path,std::ios::binary);
 std::string bytes((std::istreambuf_iterator<char>(f)),{});
 assert(bytes==reply.image);
 reply.id.hash="../etc/passwd";
 assert(bart::cacheImage(reply,argv[1],path));
 assert(path.find("..") == std::string::npos);
 reply.image="not an image";
 assert(!bart::cacheImage(reply,argv[1],path));
 reply.result=1;
 assert(!bart::cacheImage(reply,argv[1],path));
 std::cout << "BART cache tests passed\n";
}
