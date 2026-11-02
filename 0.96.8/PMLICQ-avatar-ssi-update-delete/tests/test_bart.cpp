#include "../KICQ/src/bart_codec.h"
#include <cassert>
#include <iostream>
int main() {
 bart::AssetId id; id.type=1; id.flags=1; id.hash=std::string(16,'H');
 bart::Bytes query;
 assert(bart::makeDownload("454111",id,query));
 assert(query.substr(0,8)==std::string("\x06" "454111" "\x01",8));
 bart::Bytes upload;
 assert(bart::makeUpload(1,"GIF89a!",upload));
 assert(upload.size()==11);
 assert(!bart::makeUpload(1,bart::Bytes(65536,'x'),upload));
 bart::Bytes reply;
 reply.push_back(6);reply+="454111";bart::appendId(reply,id);reply.push_back(0);bart::appendId(reply,id);
 bart::put16(reply,8);reply+="GIF89a!!";
 bart::ImageReply parsed;
 assert(bart::parseDownloadReply(reply,parsed));
 assert(parsed.image=="GIF89a!!" && parsed.id.hash==id.hash);
 reply.pop_back(); assert(!bart::parseDownloadReply(reply,parsed));
 reply.push_back('!');reply[8]=char(20);assert(!bart::parseDownloadReply(reply,parsed));
 std::cout << "BART codec tests passed\n";
}
