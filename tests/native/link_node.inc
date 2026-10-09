// Compiles link.cpp into namespace LINK_NS (see sim.h). Its headers are included first, outside the namespace, so
// their include guards keep link.cpp's own #includes from pulling them in again inside it.
#include <Arduino.h>
#include <SHA256.h>
#include "config.h"
#include "link_types.h"
#include "log.h"
#include "radio.h"
#include "sim.h"

namespace LINK_NS {
#include "link.cpp"
}

LinkApi LINK_API() {
  return { LINK_NS::linkBegin, LINK_NS::linkPoll, LINK_NS::linkSend, LINK_NS::linkSendReliable,
           LINK_NS::linkPending, LINK_NS::linkAck, LINK_NS::linkAckLater, LINK_NS::linkStats,
           LINK_NS::linkPeerVerified };
}
