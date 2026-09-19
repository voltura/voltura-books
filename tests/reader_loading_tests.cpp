#include "reader_loading.h"
#include <iostream>
using namespace books;
int main(){
    int failures=0;
    auto check=[&](bool value){if(!value)++failures;};
    ReaderLoading state;
    auto opening=state.begin(ReaderOperation::Opening,100);
    check(state.visible(100));
    check(state.begin(ReaderOperation::Opening,200)==opening); // conversion -> WebView
    check(state.started==100&&state.visible(200));
    check(!state.expired(30099)&&state.expired(30100));
    state.finish(opening);check(!state.visible(30200));
    auto page=state.begin(ReaderOperation::Page,1000);
    check(!state.visible(1149)&&state.visible(1150));
    state.finish(page);check(!state.visible(1200));
    auto fast=state.begin(ReaderOperation::Page,2000);state.finish(fast);check(!state.visible(2200));
    auto stale=state.begin(ReaderOperation::View,3000);
    state.cancel();auto current=state.begin(ReaderOperation::Opening,3001);
    check(!state.finish(stale)&&state.visible(3001));
    check(state.finish(current)&&!state.visible(3500));
    auto enter=state.begin(ReaderOperation::View,4000);
    auto exit=state.begin(ReaderOperation::View,4050);
    check(!state.finish(enter));check(!state.visible(4199)&&state.visible(4200));
    check(state.finish(exit));state.cancel();check(!state.expired(100000));
    std::cout<<"Reader loading identities, delays, transitions and cancellation: "<<failures<<" failures\n";
    return failures?1:0;
}
