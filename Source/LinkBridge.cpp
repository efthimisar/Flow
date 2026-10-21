#include "LinkBridge.h"

#if FLOW_LINK
 #include <ableton/Link.hpp>
 #include <chrono>

struct LinkBridge::Impl
{
    ableton::Link link { 120.0 };
};

LinkBridge::LinkBridge() : impl (new Impl()) {}
LinkBridge::~LinkBridge() = default;
bool LinkBridge::isAvailable() { return true; }
void LinkBridge::enable (bool on) { impl->link.enable (on); }
bool LinkBridge::isEnabled() const { return impl->link.isEnabled(); }
int  LinkBridge::numPeers() const { return (int) impl->link.numPeers(); }

void LinkBridge::capture (double latency, double& tempo, double& beat)
{
    const auto state = impl->link.captureAudioSessionState();
    const auto when = impl->link.clock().micros() + std::chrono::microseconds ((long long) (latency * 1.0e6));
    tempo = state.tempo();
    beat = state.beatAtTime (when, 4.0);
}

void LinkBridge::setTempo (double bpm)
{
    auto state = impl->link.captureAppSessionState();
    state.setTempo (bpm, impl->link.clock().micros());
    impl->link.commitAppSessionState (state);
}
#else
struct LinkBridge::Impl {};
LinkBridge::LinkBridge() = default;
LinkBridge::~LinkBridge() = default;
bool LinkBridge::isAvailable() { return false; }
void LinkBridge::enable (bool) {}
bool LinkBridge::isEnabled() const { return false; }
int  LinkBridge::numPeers() const { return 0; }
void LinkBridge::capture (double, double& tempo, double& beat) { tempo = 120.0; beat = 0.0; }
void LinkBridge::setTempo (double) {}
#endif
