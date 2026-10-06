#include "ending.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <istream>
#include <stdexcept>

namespace yami::ending {
namespace {
constexpr std::string_view initialMessage =
    "Giriş kodunuzu öğrenmek için telefon numaranızı \r\ngirip TAMAM'a basınız.";
constexpr std::string_view invalidMessage =
    "Telefon numaranız 10 adet nümerik karakterden \r\noluşmalıdır.";
constexpr std::array<unsigned char, 10> digits{{0,1,2,3,4,15,14,13,11,8}};
constexpr std::array<unsigned char, 16> permutation{{3,13,7,12,10,2,6,8,15,0,9,4,14,5,1,11}};
constexpr std::string_view alphabet = "D2PUHK3OYA4TJ5BQNIV6E7RLMCX8F9GW";
constexpr std::array<float,4> black{{0.08f,0.08f,0.08f,1}};
constexpr float xUnit = 3.5f, yUnit = 3.25f, originX = 107.75f, originY = 86.375f;
constexpr float textScale = 0.85f;
menu::Rect units(float x, float y, float w, float h) {
    return {originX+x*xUnit, originY+(177-y-h)*yUnit, w*xUnit,h*yUnit};
}
void validate_score(std::string_view score) {
    unsigned value = 0;
    const auto result = std::from_chars(score.data(),score.data()+score.size(),value);
    if (score.empty() || score.size() > 5 || result.ec != std::errc{} ||
        result.ptr != score.data()+score.size() || value < 1000 || value > 99999)
        throw std::domain_error("son.eti score format is only defined for 1000..99999; original larger scores read outside its permutation");
}
std::size_t previous(std::string_view text, std::size_t pos) {
    if (!pos) return 0;
    do { --pos; } while (pos && (static_cast<unsigned char>(text[pos])&0xc0)==0x80);
    return pos;
}
std::size_t next(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) return text.size();
    do { ++pos; } while (pos < text.size() && (static_cast<unsigned char>(text[pos])&0xc0)==0x80);
    return pos;
}
}

std::uint16_t original_random(std::uint32_t tickMilliseconds) noexcept {
    // MSVCR71.dll rand 7c366be9, not the platform-dependent std::rand().
    return static_cast<std::uint16_t>(((tickMilliseconds*214013u+2531011u)>>16)&32767u);
}
bool valid_phone(std::string_view phone) noexcept {
    return phone.size()==10 && std::all_of(phone.begin(),phone.end(),
                                         [](char c) { return c >= '0' && c <= '9'; });
}
std::string read_score(std::istream& input) {
    std::string token;
    input >> std::setw(7) >> token; // enough to reject every overlong original token
    if (!input) throw std::runtime_error("Missing or unreadable native ending score handoff");
    validate_score(token);
    return token;
}
std::string generate_code(std::string_view score, std::string_view phone,
                          std::uint16_t random) {
    validate_score(score);
    if (!valid_phone(phone)) throw std::invalid_argument("Ending phone must contain ten ASCII digits");
    if (random > 32767) throw std::domain_error("Original ending random value exceeds MSVCR71 RAND_MAX");
    std::array<unsigned char,84> bits{};
    std::size_t used = 0;
    auto nibble = [&](unsigned value) {
        for (int shift = 3; shift >= 0; --shift)
            bits[used++] = static_cast<unsigned char>((value>>shift)&1u);
    };
    auto decimal = [&](std::string_view value) {
        for (const char c : value) nibble(digits[static_cast<unsigned char>(c-'0')]);
    };
    // 00401ba0..00401c80: random decimal is zero-padded to five digits.
    std::array<char,5> randomDigits{};
    unsigned remaining = random;
    for (std::size_t i = 5; i > 0; --i) {
        randomDigits[i-1] = static_cast<char>('0'+remaining%10);
        remaining /= 10;
    }
    decimal({randomDigits.data(),randomDigits.size()});
    // 00401da1..00401dcd concatenates random bits + marker + score/phone bits.
    nibble(random < 16384 ? 6u : 10u);
    if (random < 16384) { decimal(score); decimal(phone); }
    else { decimal(phone); decimal(score); }
    std::array<unsigned char,16> groups{};
    // For five-digit scores, 00401de9 truncates the final four stream bits.
    for (std::size_t i = 0; i < used/5; ++i)
        for (std::size_t j = 0; j < 5; ++j)
            groups[i] = static_cast<unsigned char>(groups[i]*2+bits[i*5+j]);
    std::string code;
    code.reserve(19);
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (i && i%4==0) code += '-';
        code += alphabet[groups[permutation[i]]];
    }
    return code;
}

Ending::Ending(std::string_view scoreToken, std::uint32_t tickMilliseconds)
    : score_(scoreToken), message_(initialMessage), random_(original_random(tickMilliseconds)) {
    validate_score(score_);
    commands_.reserve(4);
    draws_.reserve(640);
    commands_.push_back({CommandKind::ConsumeNativeScoreHandoff,{}});
}
Ending::Ending(std::istream& handoff, std::uint32_t tickMilliseconds)
    : Ending(read_score(handoff),tickMilliseconds) {}
void Ending::set_phone(std::string_view value) {
    if (value.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Embedded NUL is not an editable phone character");
    phone_.assign(value);
    caret_ = anchor_ = focused_text().size();
    firstVisible_ = 0;
}
void Ending::submit() {
    if (!accept_enabled()) return;
    if (!valid_phone(phone_)) {
        message_.assign(invalidMessage);
        if (focus_==Control::Message) focus_control(Control::Message);
        return;
    }
    ++attempts_; // 00401ab8..00401add: disable the button at attempt three
    message_ = "Giriş kodunuz: "+generate_code(score_,phone_,random_);
    if (focus_==Control::Message) focus_control(Control::Message);
    if (!accept_enabled() && focus_==Control::Accept) focus_control(Control::Close);
}
void Ending::close() {
    if (closed_) return;
    closed_ = true;
    commands_.push_back({CommandKind::ReturnToHost,{}});
}
menu::Rect Ending::control_rect(Control control) noexcept {
    switch (control) {
    case Control::Accept: return units(33,111,75,16);
    case Control::Close: return units(127,110,74,16);
    case Control::Phone: return units(104,90,87,14);
    case Control::Score: return units(104,74,87,14);
    case Control::Message: return units(7,131,217,39);
    }
    return {};
}
std::string_view Ending::focused_text() const noexcept {
    if (focus_==Control::Phone) return phone_;
    if (focus_==Control::Score) return score_;
    if (focus_==Control::Message) return message_;
    return {};
}
void Ending::focus_control(Control value) {
    focus_ = value;
    caret_ = anchor_ = focused_text().size();
    firstVisible_ = 0;
    dragging_ = false;
}
void Ending::move_caret(std::size_t pos, bool extend) {
    caret_ = std::min(pos,focused_text().size());
    if (!extend) anchor_ = caret_;
}
void Ending::erase_selection() {
    const auto start = std::min(caret_,anchor_), count = std::max(caret_,anchor_)-start;
    phone_.erase(start,count);
    caret_ = anchor_ = start;
    firstVisible_ = std::min(firstVisible_,caret_);
}
void Ending::update(const Input& input, const menu::BitmapFont& font) {
    if (closed_) return;
    constexpr std::array<Control,5> controls{{Control::Accept,Control::Close,Control::Phone,
                                            Control::Score,Control::Message}};
    const menu::Rect closeBox{originX+231*xUnit-24,originY+177*yUnit,24,24};
    if (input.primaryPressed) {
        buttonPressed_ = false;
        closeBoxPressed_ = menu::contains(closeBox,input.pointer);
        for (const auto c : controls) {
            if ((c==Control::Accept && !accept_enabled()) ||
                !menu::contains(control_rect(c),input.pointer)) continue;
            if (!(input.shift && focus_==c)) focus_control(c);
            if (c==Control::Accept || c==Control::Close) {
                buttonPressed_ = true;
                pressedControl_ = c;
            } else dragging_ = true;
            break;
        }
    }
    if (input.primaryReleased) {
        if (closeBoxPressed_ && menu::contains(closeBox,input.pointer)) close();
        if (buttonPressed_ && menu::contains(control_rect(pressedControl_),input.pointer)) {
            if (pressedControl_==Control::Accept) submit();
            else close();
        }
        buttonPressed_ = closeBoxPressed_ = false;
    }
    if (closed_) return;
    if (dragging_ && (input.primaryPressed || input.primaryDown || input.primaryReleased)) {
        const auto value = focused_text();
        const auto rect = control_rect(focus_);
        // Phone/score are single-line ANSI edit controls in the original dialog.
        // Message remains read-only and supports explicit select-all/copy.
        if (focus_!=Control::Message) {
            const float x = input.pointer.x-rect.x-5;
            std::size_t pos = firstVisible_;
            while (pos < value.size()) {
                const auto end = next(value,pos);
                const float middle = (menu::text_width(font,value.substr(firstVisible_,pos-firstVisible_))+
                    menu::text_width(font,value.substr(firstVisible_,end-firstVisible_)))*textScale*0.5f;
                if (x < middle) break;
                pos = end;
            }
            move_caret(pos,!input.primaryPressed || input.shift);
        }
    }
    if (input.primaryReleased) dragging_ = false;
    const auto value = focused_text();
    switch (input.key) {
    case Key::None: break;
    case Key::Tab: {
        auto index = static_cast<int>(std::find(controls.begin(),controls.end(),focus_)-controls.begin());
        do { index = (index+(input.shift ? 4 : 1))%5; }
        while (controls[static_cast<std::size_t>(index)]==Control::Accept && !accept_enabled());
        focus_control(controls[static_cast<std::size_t>(index)]);
        if (focus_==Control::Phone || focus_==Control::Score || focus_==Control::Message) anchor_ = 0;
        break;
    }
    case Key::Enter: if (focus_==Control::Close) close(); else submit(); break;
    case Key::Escape: close(); break;
    case Key::SelectAll: anchor_ = 0; caret_ = value.size(); break;
    case Key::Copy:
        if (anchor_!=caret_)
            commands_.push_back({CommandKind::CopySelection,
                                std::string(value.substr(std::min(anchor_,caret_),
                                            std::max(anchor_,caret_)-std::min(anchor_,caret_)))});
        break;
    case Key::Home: move_caret(0,input.shift); break;
    case Key::End: move_caret(value.size(),input.shift); break;
    case Key::Left:
        move_caret(!input.shift && anchor_!=caret_ ? std::min(anchor_,caret_) : previous(value,caret_),input.shift);
        break;
    case Key::Right:
        move_caret(!input.shift && anchor_!=caret_ ? std::max(anchor_,caret_) : next(value,caret_),input.shift);
        break;
    case Key::Backspace:
        if (focus_==Control::Phone) {
            if (anchor_==caret_) anchor_ = previous(phone_,caret_);
            erase_selection();
        }
        break;
    case Key::Delete:
        if (focus_==Control::Phone) {
            if (anchor_==caret_) anchor_ = next(phone_,caret_);
            erase_selection();
        }
        break;
    }
    if (!closed_ && focus_==Control::Phone && !input.text.empty()) {
        if (input.text.find('\0') != std::string_view::npos ||
            input.text.find_first_of("\r\n") != std::string_view::npos) return;
        erase_selection();
        phone_.insert(caret_,input.text);
        caret_ += input.text.size();
        anchor_ = caret_;
    }
}
void Ending::rectangle(menu::Rect rect, std::array<float,4> color) {
    menu::Draw draw;
    draw.rect = {rect.x/1024,rect.y/768,rect.width/1024,rect.height/768};
    const auto r = draw.rect;
    draw.positions = {{{r.x,r.y+r.height},{r.x+r.width,r.y+r.height},
                       {r.x+r.width,r.y},{r.x,r.y}}};
    draw.uv = {{{0,1},{1,1},{1,0},{0,0}}};
    draw.color = color;
    draws_.push_back(draw);
}
void Ending::text(menu::Rect rect, std::string_view value, const menu::BitmapFont& font,
                  std::array<float,4> color, bool centered) {
    float y = rect.y+rect.height-static_cast<float>(font.height)*textScale;
    while (!value.empty() && y >= rect.y) {
        std::size_t length = 0, lastSpace = std::string_view::npos;
        while (length < value.size() && value[length]!='\r' && value[length]!='\n') {
            const auto end = next(value,length);
            if (menu::text_width(font,value.substr(0,end))*textScale > rect.width) break;
            if (value[length]==' ') lastSpace = length;
            length = end;
        }
        if (!length && value[0]!='\r' && value[0]!='\n') length = next(value,0);
        if (length < value.size() && value[length]!='\r' && value[length]!='\n' &&
            lastSpace!=std::string_view::npos) length = lastSpace;
        const auto line = value.substr(0,length);
        const float x = rect.x+(centered ? (rect.width-menu::text_width(font,line)*textScale)*0.5f : 0);
        menu::append_text(draws_,font,line,{x,y},textScale,color);
        value.remove_prefix(length);
        if (!value.empty() && value.front()==' ') value.remove_prefix(1);
        if (!value.empty() && value.front()=='\r') value.remove_prefix(1);
        if (!value.empty() && value.front()=='\n') value.remove_prefix(1);
        y -= static_cast<float>(font.height)*textScale+3;
    }
}
std::span<const menu::Draw> Ending::draw(const menu::BitmapFont& font) {
    draws_.clear();
    if (closed_) return draws_;
    rectangle({0,0,1024,768},{{0,0,0,0.7f}});
    rectangle({originX-2,originY-2,231*xUnit+4,177*yUnit+28},{{0.15f,0.15f,0.15f,1}});
    rectangle({originX,originY,231*xUnit,177*yUnit},{{0.88f,0.88f,0.86f,1}});
    rectangle({originX,originY+177*yUnit,231*xUnit,24},{{0.07f,0.17f,0.29f,1}});
    text({originX+6,originY+177*yUnit,231*xUnit-30,24},title,font,{{1,1,1,1}});
    text({originX+231*xUnit-24,originY+177*yUnit,24,24},"X",font,{{1,1,1,1}},true);
    text(units(7,8,217,9),"TEBRİKLER!",font,black);
    text(units(7,16,217,9),"Eti Yami Mekanik İstila Oyunu'nu başarıyla tamamladınız.",font,black);
    text(units(7,26,216,28),"Ödül kazanmak ve www.etietieti.com adresinde en yüksek puanlı oyuncular listesine girmek için size erişebileceğimiz telefon numaranızı girip TAMAM'a basınız.",font,black);
    text(units(7,51,212,16),"Sonra aşağıdaki mesaj kutusunda görünecek olan giriş kodunu www.etietieti.com/yami-mekanikistila adresine girmeniz yeterli.",font,black);
    text(units(56,91,42,8),"Telefonunuz:",font,black);
    text(units(69,78,30,8),"Puanınız:",font,black);
    text(units(29,99,68,8),"(örnek: 5555555550)",font,black);
    for (const auto c : {Control::Accept,Control::Close,Control::Phone,Control::Score,Control::Message}) {
        const auto r = control_rect(c);
        rectangle(r,focus_==c ? std::array<float,4>{{0.07f,0.22f,0.38f,1}} : std::array<float,4>{{0.4f,0.4f,0.4f,1}});
        const menu::Rect inside{r.x+2,r.y+2,r.width-4,r.height-4};
        const bool button = c==Control::Accept || c==Control::Close;
        rectangle(inside,button ? std::array<float,4>{{0.81f,0.81f,0.79f,1}} : std::array<float,4>{{1,1,1,1}});
        const auto color = c==Control::Accept && !accept_enabled() ? std::array<float,4>{{0.5f,0.5f,0.5f,1}} : black;
        std::string_view value = c==Control::Accept ? "TAMAM" : c==Control::Close ? "KAPAT" :
                                c==Control::Phone ? std::string_view(phone_) :
                                c==Control::Score ? std::string_view(score_) : std::string_view(message_);
        if (!button && c!=Control::Message) {
            std::size_t first = c==focus_ ? std::min(firstVisible_,caret_) : 0;
            while (c==focus_ && menu::text_width(font,value.substr(first,caret_-first))*textScale > inside.width-10)
                first = next(value,first);
            if (c==focus_) firstVisible_ = first;
            std::size_t end = first;
            while (end < value.size() && menu::text_width(font,value.substr(first,next(value,end)-first))*textScale <= inside.width-10)
                end = next(value,end);
            if (c==focus_ && anchor_!=caret_) {
                const auto lo = std::clamp(std::min(anchor_,caret_),first,end);
                const auto hi = std::clamp(std::max(anchor_,caret_),first,end);
                const float x = inside.x+3+menu::text_width(font,value.substr(first,lo-first))*textScale;
                rectangle({x,inside.y+3,menu::text_width(font,value.substr(lo,hi-lo))*textScale,inside.height-6},{{0.65f,0.8f,0.94f,1}});
            }
            if (c==focus_ && c==Control::Phone && caret_>=first && caret_<=end)
                rectangle({inside.x+3+menu::text_width(font,value.substr(first,caret_-first))*textScale,
                           inside.y+4,1,inside.height-8},black);
            value = value.substr(first,end-first);
        }
        text({inside.x+3,inside.y+3,inside.width-6,inside.height-6},value,font,color,button);
    }
    return draws_;
}
} // namespace yami::ending
