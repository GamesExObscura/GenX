#pragma once

#include <array>

#include "mercury/genesis/genesis.hpp"
#include "mercury/net/netplay.hpp"

namespace mercury::net {

// THE TWO SEATS OF A NETPLAY MEGA DRIVE, PUT INTO ITS PORTS.
//
// Both ends call this with the same two inputs every frame, so it is the one
// place that decides which pins a player's buttons and gun reach - shared by
// the shell and by netplay_check, so the tool proves the routing the program
// actually uses rather than a copy of it.
//
// The routing is the console's, not "player one to port one":
//   - a light gun is always in PORT 2 (the Menacer's receiver box goes there,
//     and the Justifier is cabled there), whichever seat holds it;
//   - two Justifiers are one port: seat 2's gun is the second gun daisy-chained
//     off the first, with its own aim, trigger and Start;
//   - a gun's trigger is the A line, and its reload is a trigger pull with the
//     gun pointed away (the sender has already marked the aim off screen).
//
// LEVEL, NOT EDGE. Every button on both ports is set every frame, including
// the ones nobody holds, because a pin left as this computer's own keyboard
// last set it is a pin the other computer does not have. Two seats that land
// on the same port are OR'd, as two hands on one pad would be.
//
// The ports' KINDS must already be set the same at both ends (the shell does
// it from Session::seat_kinds before the first frame); this only drives them.
void apply_genesis_seats(genesis::Genesis& machine,
                         const std::array<genesis::ControllerPort::Kind, 2>& seats,
                         const std::array<PlayerInput, 2>& inputs) noexcept;

}  // namespace mercury::net
