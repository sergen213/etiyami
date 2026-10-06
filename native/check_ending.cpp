#include "ending.hpp"

#include <cassert>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    using namespace yami::ending;
    // Clearly dummy phone inputs only; no personal data or promotion submission.
    assert(original_random(1)==41);
    assert(original_random(0)==38);
    assert(valid_phone("0000000000"));
    assert(!valid_phone("000000000"));
    assert(!valid_phone("00000000000"));
    assert(!valid_phone("000000000a"));
    assert(!valid_phone("００００００００００"));
    // Address-level recovery vectors in analysis/ending/evidence.json cover
    // both sides of 00401af6's random-order boundary and 4/5 digit scores.
    assert(generate_code("1000","0000000000",0)=="DDDD-DDDD-DDDJ-DHDD");
    assert(generate_code("99999","0000000000",32767)=="5PD2-DQDD-Y3DE-HDTD");
    assert(generate_code("12345","0123456789",16383)=="6WEA-A9IG-8UDJ-RHMU");
    assert(generate_code("12345","0123456789",16384)=="EHUN-R9AA-EUWE-IDM8");
    for (const char* bad : {"", "999", "100000", "-1000", "10a00"}) {
        bool rejected = false;
        try { static_cast<void>(generate_code(bad,"0000000000",0)); }
        catch (const std::domain_error&) { rejected = true; }
        assert(rejected);
    }
    std::istringstream handoff("\r\n1000\nignored-by-original-fscanf");
    Ending state(handoff,1);
    assert(state.score()=="1000" && state.random()==41 && state.attempts()==0);
    assert(state.focus()==Control::Accept);
    assert(state.commands().size()==1);
    assert(state.commands()[0].kind==CommandKind::ConsumeNativeScoreHandoff);
    state.clear_commands();
    state.set_phone("invalid");
    state.submit();
    assert(state.attempts()==0 && state.accept_enabled());
    assert(state.message()=="Telefon numaranız 10 adet nümerik karakterden \r\noluşmalıdır.");
    state.set_phone("0000000000");
    state.submit();
    const std::string firstCode(state.message());
    state.submit();
    assert(state.attempts()==2 && state.message()==firstCode);
    state.submit();
    assert(state.attempts()==3 && !state.accept_enabled());
    state.set_phone("0123456789");
    state.submit();
    assert(state.attempts()==3 && state.message()==firstCode);
    assert(state.commands().empty()); // no URL launch, network, print or file of phone data
    state.close();
    state.close();
    assert(state.closed() && state.commands().size()==1);
    assert(state.commands()[0].kind==CommandKind::ReturnToHost);

    const auto fonts = yami::menu::read_fonts(argc > 1 ? std::filesystem::path(argv[1]) : "game");
    assert(!fonts.empty());
    const auto& font = fonts.front();
    Ending edit("1000",1);
    edit.clear_commands();
    Input tab;
    tab.key = Key::Tab;
    edit.update(tab,font); // default TAMAM -> KAPAT -> phone
    edit.update(tab,font);
    assert(edit.focus()==Control::Phone);
    Input typing;
    typing.text = "0000000000";
    edit.update(typing,font);
    assert(edit.phone()=="0000000000");
    Input select;
    select.key = Key::SelectAll;
    edit.update(select,font);
    typing.text = "0123456789";
    edit.update(typing,font);
    assert(edit.phone()=="0123456789");
    Input accept;
    accept.key = Key::Enter;
    edit.update(accept,font);
    assert(edit.attempts()==1);
    edit.update(tab,font); // score is read-only
    typing.text = "9";
    edit.update(typing,font);
    assert(edit.score()=="1000");
    edit.update(tab,font); // message is read-only, supports user copy
    edit.update(select,font);
    Input copy;
    copy.key = Key::Copy;
    edit.update(copy,font);
    assert(edit.commands().size()==1 && edit.commands()[0].kind==CommandKind::CopySelection);
    assert(edit.commands()[0].text==edit.message());
    const auto draws = edit.draw(font);
    assert(!draws.empty());
    bool glyph = false;
    for (const auto& d : draws) {
        glyph |= d.glyph;
        for (const auto p : d.positions) assert(p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1);
    }
    assert(glyph);
    Input escape;
    escape.key = Key::Escape;
    edit.update(escape,font);
    assert(edit.closed() && edit.draw(font).empty());
    Ending pointer("1000",1);
    pointer.set_phone("0000000000");
    const auto acceptRect = Ending::control_rect(Control::Accept);
    Input press;
    press.pointer = {acceptRect.x+5,acceptRect.y+5};
    press.primaryPressed = press.primaryDown = true;
    pointer.update(press,font);
    assert(pointer.attempts()==0); // MFC buttons act on release, not press
    Input release;
    release.primaryReleased = true;
    release.pointer = {0,0};
    pointer.update(release,font);
    assert(pointer.attempts()==0); // cancelling a captured button press
    pointer.update(press,font);
    release.pointer = press.pointer;
    pointer.update(release,font);
    assert(pointer.attempts()==1);
}
