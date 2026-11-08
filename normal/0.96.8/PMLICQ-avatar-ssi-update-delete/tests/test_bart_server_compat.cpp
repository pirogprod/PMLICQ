#include "../KICQ/src/bart_codec.h"
#include "../KICQ/src/bart_ssi.h"
#include <cassert>
#include <iostream>
int main() {
    // bart.rs SNAC 10/03: success byte (not uint16), then BART ID.
    bart::Bytes hash(16, '\x47'), reply(1, '\0');
    bart::AssetId requested; requested.type=1; requested.flags=1; requested.hash=hash;
    assert(bart::appendId(reply, requested));
    assert(reply.size() == 21);
    size_t i=0; uint8_t code=255; bart::AssetId parsed;
    assert(bart::get8(reply,i,code) && code==0);
    assert(bart::parseId(reply,i,parsed) && i==reply.size());
    assert(parsed.type==1 && parsed.hash==hash);

    // bart.rs SNAC 10/07: screen name, requested id, byte result, returned id, length, image.
    bart::Bytes body; body.push_back(6); body += "712003";
    assert(bart::appendId(body, requested));
    body.push_back(0);
    assert(bart::appendId(body, requested));
    const bart::Bytes img="GIF89a0123456789";
    bart::put16(body, uint16_t(img.size())); body += img;
    bart::ImageReply decoded;
    assert(bart::parseDownloadReply(body, decoded));
    assert(decoded.screenName == "712003" && decoded.id.hash == hash && decoded.image == img);

    // SSI type 0x14 icon MD5: 00D5 = [flags=1][hashlen=16][MD5].
    bartssi::Bytes ssi;
    assert(bartssi::iconItem(0x4000,hash,ssi));
    assert(ssi.size() >= 22 && ssi.substr(ssi.size()-16) == hash);
    std::cout << "OSCAR BART server compatibility fixtures passed\n";
}
