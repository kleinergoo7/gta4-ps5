#pragma once

#include <cstdint>

// WORKAROUND: commit-on-demand for guard pages inside the guest arena.
// The game derives some texture-streaming upload destinations from stack
// slots its own call chain never stores to; on real hardware (dense RAM)
// those stale dests land mapped and the copies succeed silently, while our
// sparse arena (PROT_NONE guard stripes) turns them fatal. Committing the
// page mirrors hardware. Proper fix: find why those stack slots/registers
// are uninitialized (see step-6 investigation) instead of masking it here.
// Every commit and every decline is logged; the 256MB budget and the
// same-page guard keep a genuine bug from being masked silently.
namespace ArenaGuard
{

// Install the raw chained SIGSEGV handler. Must be called after the guest
// arena base is set. Chains to the previously installed handler on decline.
void Install();

}  // namespace ArenaGuard
