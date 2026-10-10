// link.h for the test build: its types, with the function declarations renamed out of the way. link.cpp is
// compiled into a namespace per node (link_node.inc); were ::linkSend and the rest declared too, argument-dependent
// lookup (MsgType is a global enum) would find both and the calls inside link.cpp would be ambiguous.
#pragma once
#define linkSetBoot unused_linkSetBoot
#define linkBegin unused_linkBegin
#define linkPoll unused_linkPoll
#define linkSend unused_linkSend
#define linkSendReliable unused_linkSendReliable
#define linkPending unused_linkPending
#define linkAck unused_linkAck
#define linkAckLater unused_linkAckLater
#define linkRefuse unused_linkRefuse
#define linkStats unused_linkStats
#define linkPeerVerified unused_linkPeerVerified
#define linkPeerSession unused_linkPeerSession
#define linkDebugReplay unused_linkDebugReplay
#define linkDebugMute unused_linkDebugMute
#include "link.h"
#undef linkSetBoot
#undef linkBegin
#undef linkPoll
#undef linkSend
#undef linkSendReliable
#undef linkPending
#undef linkAck
#undef linkAckLater
#undef linkRefuse
#undef linkStats
#undef linkPeerVerified
#undef linkPeerSession
#undef linkDebugReplay
#undef linkDebugMute
