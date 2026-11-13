#pragma once

// Network play for two humans: one hosts, the other joins, over a WebSocket (Qt's, in Backend/Qt/NetPlay.cpp). The game runs in lockstep on both machines (the Mega Drive games are deterministic: the same
// start and the same pad inputs give the same game), so only the pads travel, a frame number and a button byte each, never the picture. The host is the first player and the guest the second: the game feeds
// what NetPlay_Frame gives into its two pads and turns versus on (Sonic 2's `true_two_player`: versus is really active, which the split screen alone does not say). Network play does not use the split screen: each
// machine draws the one view, the game decides whose.
//
// The host makes a password (or is given one) and passes it to the guest by whatever way they like. The password never goes over the wire: the host sends a random number, the guest answers with a proof made
// from the password and that number, and the host proves itself back (so neither side talks to an impostor). Every message after that carries a short MAC of a key the two have made from the password and both
// numbers, and a count that must go up by one, so a message cannot be changed, made up or sent again by someone on the way (though the WebSocket is plain: what travels is not secret, and a stranger can still
// cut the connection). A guest that does not know the password is refused, and so is a second guest.
//
//   host:   n = NetPlay_Create(); NetPlay_Host(n, "sonic2", "0.0.0.0", 0, NULL, 4); show NetPlay_Password(n) and NetPlay_Port(n); poll until NETPLAY_READY; NetPlay_StartMatch(n, blob, size)
//   guest:  n = NetPlay_Create(); NetPlay_Join(n, "sonic2", "address", port, "the password"); poll until NETPLAY_RUNNING (the match's start data is NetPlay_StartData)
//   both:   each game frame: NetPlay_Poll(n); if (NetPlay_Frame(n, my_pad, &pad1, &pad2)) { run the frame with pad1 and pad2 } else { wait (the other side's input has not come) }
//           every NetPlay_HashDue(n): NetPlay_ReportHash(n, hash of the game's state); a difference ends the match as a desync

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NetPlay NetPlay;

typedef enum {
    NETPLAY_IDLE,       // nothing started
    NETPLAY_LISTENING,  // host: waiting for the guest
    NETPLAY_CONNECTING, // guest: connecting and proving it knows the password
    NETPLAY_READY,      // the other side is there and proved itself; the host has not started the match
    NETPLAY_RUNNING,    // the match is on (lockstep)
    NETPLAY_CLOSED      // over, NetPlay_Reason says why (a refused password, a dropped connection, a desync, or a normal end)
} NetPlayStatus;

NetPlay *NetPlay_Create(void);
void NetPlay_Destroy(NetPlay *n); // (ends the session too)

// Host: listens on `bind_address` (NULL: every address) and `port` (0: any free one, see NetPlay_Port). `game` names the game and its version: both sides must give the same. `password` NULL makes a random one
// (NetPlay_Password; 20 letters and digits in groups of 4); the letters' case and the dashes and spaces do not matter. `input_delay` is how many frames the pads are held back so that the other side's has
// time to arrive (0 to 15; 4 is about a fifteenth of a second at 60 frames a second).
bool NetPlay_Host(NetPlay *n, const char *game, const char *bind_address, uint16_t port, const char *password, int input_delay);
// Guest: connects to the host and proves the password (the answer comes while polling: NETPLAY_READY or NETPLAY_CLOSED with the reason)
bool NetPlay_Join(NetPlay *n, const char *game, const char *host, uint16_t port, const char *password);
// Ends the session (the other side is told)
void NetPlay_Stop(NetPlay *n);

// Lets the sockets work: call once a frame (and in any wait). It does not block.
void NetPlay_Poll(NetPlay *n);

NetPlayStatus NetPlay_Status(const NetPlay *n);
bool NetPlay_IsHost(const NetPlay *n);
int NetPlay_LocalPlayer(const NetPlay *n);   // 0 the host (the first player), 1 the guest
const char *NetPlay_Password(const NetPlay *n); // host: the password to give the guest
uint16_t NetPlay_Port(const NetPlay *n);        // host: the port it listens on
const char *NetPlay_Reason(const NetPlay *n);   // why it is closed (or the last thing that went wrong)
int NetPlay_InputDelay(const NetPlay *n);
int NetPlay_PingMs(const NetPlay *n);           // the round trip, -1 until measured

// Host, once NETPLAY_READY: starts the match on both machines. `data` is what the guest needs to start the same game (the level, the random seed, the options: the game decides, up to 4096 bytes).
bool NetPlay_StartMatch(NetPlay *n, const uint8_t *data, size_t size);
// Guest, once NETPLAY_RUNNING: what the host started it with (valid until the session ends)
const uint8_t *NetPlay_StartData(const NetPlay *n, size_t *size);

// One game frame: `local_pad` is the pad now (it is used `input_delay` frames on); true gives the two pads of the frame to run (the host's and the guest's) and moves the frame on, false says the other side's
// input for the frame has not come: do not run the frame, ask again. Only while NETPLAY_RUNNING.
bool NetPlay_Frame(NetPlay *n, uint8_t local_pad, uint8_t *pad1, uint8_t *pad2);
uint32_t NetPlay_FrameNumber(const NetPlay *n); // the frame the next NetPlay_Frame gives
// Every 60th frame the two sides compare a hash of the game's state: when it is due (before running that frame), the game reports its own
bool NetPlay_HashDue(const NetPlay *n);
void NetPlay_ReportHash(NetPlay *n, uint32_t hash);
bool NetPlay_Desynced(const NetPlay *n, uint32_t *frame);

#ifdef __cplusplus
}
#endif
