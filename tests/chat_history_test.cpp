#include "chat_history.h"
#include <stdexcept>
static void check(bool b) { if (!b) throw std::runtime_error("History budget violated"); }
int main() {
    ChatHistory history;
    for (int i = 1; i <= 50000; ++i) {
        check(history.Append({i,0,"alice",std::string(1000,'x')}));
        check(history.size() <= ChatHistory::MaxMessages);
        check(history.AccountedBytes() <= ChatHistory::MaxAccountedBytes);
    }
    check(history[history.size()-1].id == 50000);
    check(history.FirstOrdinal() + history.size() == 50000);
    auto revision = history.Revision();
    history.Clear(); check(history.empty() && history.AccountedBytes() == 0 && history.FirstOrdinal() == 0);
    check(history.Revision() > revision);
    for (int i = 1; i <= 10000; ++i) {
        check(history.Append({i,0,"alice",std::string(2000,'\n')}));
        check(history.AccountedBytes() <= ChatHistory::MaxAccountedBytes);
    }
    // Newline-heavy messages need their row metadata accounted for, not just bytes of text.
    check(history.size() <= 9 && history[history.size()-1].id == 10000);
    auto size = history.size();
    std::string oversizedCapacity; oversizedCapacity.reserve(ChatHistory::MaxAccountedBytes*2);
    oversizedCapacity = "small contents";
    check(!history.Append({10001,0,"alice",std::move(oversizedCapacity)}));
    check(history.size() == size);
    check(!history.Append({10001,0,"alice",std::string(ChatHistory::MaxAccountedBytes,'x')}));
    check(history.size() == size);
    history.Clear();
    for (int i = 10001; i <= 10512; ++i) history.Append({i,0,"alice","text"});
    // Move a full window all the way back, evicting newer rows, then forwards.
    for (int i = 10000; i >= 1; --i) {
        check(history.Prepend({i,0,"alice","text"}));
        check(history.size() <= ChatHistory::MaxMessages && history.AccountedBytes() <= ChatHistory::MaxAccountedBytes);
    }
    check(history[0].id == 1 && history[history.size()-1].id == 512);
    check(!history.Prepend({512,0,"alice","duplicate"}));
    for (int i = 513; i <= 10512; ++i) history.Append({i,0,"alice","text"});
    check(history[0].id == 10001 && history[history.size()-1].id == 10512);

}
